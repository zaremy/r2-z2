/* Host tests for the read-only ops (#114 slice 3).
 *
 * Refusals first, legal cases after -- the shape this repo learned the hard
 * way (CLAUDE.md, "Mutate the GUARD, not the table"). A parser that decodes a
 * correct frame correctly is the easy half; the half that matters is that it
 * REFUSES a frame belonging to another op, because responses all arrive on one
 * notification characteristic and nothing but this check separates them.
 */
#include <stdio.h>
#include <string.h>

#include "r2_ops.h"

static int failures = 0, checks = 0;

#define CHECK(cond, ...) do {                                \
    checks++;                                                \
    if (!(cond)) { failures++;                               \
        printf("  FAIL: "); printf(__VA_ARGS__);             \
        printf("\n        at %s:%d\n", __FILE__, __LINE__); }\
} while (0)

static int    tx_calls;
static size_t tx_len;
static uint8_t tx_buf[64];

static int fake_tx(const uint8_t *f, size_t n, void *ctx)
{
    (void)ctx;
    tx_calls++;
    tx_len = n < sizeof tx_buf ? n : sizeof tx_buf;
    memcpy(tx_buf, f, tx_len);
    return 0;
}

static r2_response_t resp(uint8_t did, uint8_t cid, uint8_t err,
                          const uint8_t *data, size_t len)
{
    r2_response_t r;
    r.flags = 0x09; r.did = did; r.cid = cid; r.seq = 0x13;
    r.err = err; r.data = data; r.data_len = len;
    return r;
}

/* ---- REFUSALS ---------------------------------------------------------- */

/* The one that matters most. A head response is four bytes of float; fed to
 * the battery parser without a did/cid check it decodes as a plausible
 * voltage and nothing anywhere ever notices. */
static void test_parser_refuses_another_ops_response(void)
{
    const uint8_t head_payload[4] = { 0x42, 0x28, 0x00, 0x00 };  /* 42.0 deg */
    const uint8_t batt_payload[2] = { 0x01, 0xBA };

    r2_battery_t b; r2_head_t h; r2_version_t v;

    /* head frame -> battery parser */
    r2_response_t r = resp(0x17, 0x14, 0, head_payload, 4);
    CHECK(r2_ops_parse_battery(&r, &b) == R2_OPS_WRONG_OP,
          "battery parser accepted a head response");

    /* battery frame -> head parser */
    r = resp(0x13, 0x03, 0, batt_payload, 2);
    CHECK(r2_ops_parse_head(&r, &h) == R2_OPS_WRONG_OP,
          "head parser accepted a battery response");

    /* battery frame -> version parser */
    CHECK(r2_ops_parse_version(&r, &v) == R2_OPS_WRONG_OP,
          "version parser accepted a battery response");

    /* Right CID, wrong DID -- catches a check that only compares one half. */
    r = resp(0x11, 0x03, 0, batt_payload, 2);
    CHECK(r2_ops_parse_battery(&r, &b) == R2_OPS_WRONG_OP,
          "battery parser matched on cid alone (did ignored)");

    /* Right DID, wrong CID -- the mirror of the above. */
    r = resp(0x13, 0x0D, 0, batt_payload, 2);
    CHECK(r2_ops_parse_battery(&r, &b) == R2_OPS_WRONG_OP,
          "battery parser matched on did alone (cid ignored)");
}

/* R2 answering "bad_command_id" is the EXPECTED outcome for the version probe.
 * If that decoded as version 0.0.0 we would ship a fiction, which is exactly
 * gap B3's "FW 0.4.1" all over again. */
static void test_parser_refuses_a_device_error(void)
{
    const uint8_t payload[6] = { 0, 0, 0, 0, 0, 0 };
    r2_version_t v;
    r2_response_t r = resp(0x11, 0x00, 0x02 /* bad_command_id */, payload, 6);
    CHECK(r2_ops_parse_version(&r, &v) == R2_OPS_DEVICE_ERROR,
          "version parser decoded an error response as a version");

    r2_battery_t b;
    const uint8_t bp[2] = { 0x01, 0xBA };
    r = resp(0x13, 0x03, 0x06 /* command_failed */, bp, 2);
    CHECK(r2_ops_parse_battery(&r, &b) == R2_OPS_DEVICE_ERROR,
          "battery parser decoded an error response as volts");

    /* And the error must be reported as itself, not folded into a length or
     * op complaint -- the code is the whole finding. */
    CHECK(strcmp(r2_ops_device_error_name(0x02), "bad_command_id") == 0,
          "0x02 should name bad_command_id");
    CHECK(strcmp(r2_ops_device_error_name(0xEE), "unknown_error") == 0,
          "an unlisted code should not silently name a real one");
}

/* A short payload read past its end is how a parser invents data. */
static void test_parser_refuses_a_wrong_length(void)
{
    const uint8_t one[1]   = { 0x01 };
    const uint8_t three[3] = { 0x42, 0x28, 0x00 };
    const uint8_t five[5]  = { 0, 1, 0, 2, 0 };
    const uint8_t seven[7] = { 0, 1, 0, 2, 0, 3, 0 };
    r2_battery_t b; r2_head_t h; r2_version_t v;

    r2_response_t r = resp(0x13, 0x03, 0, one, 1);
    CHECK(r2_ops_parse_battery(&r, &b) == R2_OPS_BAD_LENGTH, "battery took 1 byte");
    r = resp(0x13, 0x03, 0, NULL, 0);
    CHECK(r2_ops_parse_battery(&r, &b) == R2_OPS_BAD_LENGTH, "battery took 0 bytes");
    r = resp(0x13, 0x03, 0, three, 3);
    CHECK(r2_ops_parse_battery(&r, &b) == R2_OPS_BAD_LENGTH, "battery took 3 bytes");

    r = resp(0x17, 0x14, 0, three, 3);
    CHECK(r2_ops_parse_head(&r, &h) == R2_OPS_BAD_LENGTH, "head took 3 bytes");
    r = resp(0x17, 0x14, 0, five, 5);
    CHECK(r2_ops_parse_head(&r, &h) == R2_OPS_BAD_LENGTH, "head took 5 bytes");

    r = resp(0x11, 0x00, 0, five, 5);
    CHECK(r2_ops_parse_version(&r, &v) == R2_OPS_BAD_LENGTH, "version took 5 bytes");
    r = resp(0x11, 0x00, 0, seven, 7);
    CHECK(r2_ops_parse_version(&r, &v) == R2_OPS_BAD_LENGTH, "version took 7 bytes");
}

static void test_parser_refuses_nulls(void)
{
    /* A response that claims a payload and carries no pointer. The length check
     * passes, so without a pointer check every parser dereferences NULL. */
    r2_response_t claims2 = resp(0x13, 0x03, 0, NULL, 2);
    r2_battery_t  b2;
    CHECK(r2_ops_parse_battery(&claims2, &b2) == R2_OPS_NULL,
          "battery parser dereferenced a null payload of claimed length 2");
    r2_response_t claims4 = resp(0x17, 0x14, 0, NULL, 4);
    r2_head_t h4;
    CHECK(r2_ops_parse_head(&claims4, &h4) == R2_OPS_NULL,
          "head parser dereferenced a null payload of claimed length 4");
    r2_response_t claims6 = resp(0x11, 0x00, 0, NULL, 6);
    r2_version_t v6;
    CHECK(r2_ops_parse_version(&claims6, &v6) == R2_OPS_NULL,
          "version parser dereferenced a null payload of claimed length 6");

    const uint8_t bp[2] = { 0x01, 0xBA };
    r2_response_t r = resp(0x13, 0x03, 0, bp, 2);
    r2_battery_t b; r2_head_t h; r2_version_t v;

    CHECK(r2_ops_parse_battery(NULL, &b)  == R2_OPS_NULL, "battery took a null response");
    CHECK(r2_ops_parse_battery(&r, NULL)  == R2_OPS_NULL, "battery took a null out");
    CHECK(r2_ops_parse_head(NULL, &h)     == R2_OPS_NULL, "head took a null response");
    CHECK(r2_ops_parse_head(&r, NULL)     == R2_OPS_NULL, "head took a null out");
    CHECK(r2_ops_parse_version(NULL, &v)  == R2_OPS_NULL, "version took a null response");
    CHECK(r2_ops_parse_version(&r, NULL)  == R2_OPS_NULL, "version took a null out");
}

/* ---- ROUTING: the gate must be the only way out ------------------------- */

/* Nothing about a READ-tier op can be refused by the ceiling, so the ceiling
 * cannot witness the routing. These two can:
 *
 *  - a NULL tx returns the GATE's verdict (-4 R2_GATE_NO_TX). Calling
 *    r2_packet_encode directly would return -6 (R2_ERR_NULL) or scribble.
 *  - every did/cid this module sends must be a name the gate knows. An op the
 *    gate has never heard of is refused NOT_ALLOWLISTED at runtime, so a
 *    request built here for an unlisted op would be dead on arrival. */
static void test_requests_route_through_the_gate(void)
{
    tx_calls = 0;
    CHECK(r2_ops_request_battery(0x13, NULL, NULL) == R2_GATE_NO_TX,
          "battery request did not return the gate's no-tx verdict");
    CHECK(r2_ops_request_head(0x13, NULL, NULL) == R2_GATE_NO_TX,
          "head request did not return the gate's no-tx verdict");
    CHECK(r2_ops_probe_version(0x13, NULL, NULL) == R2_GATE_NO_TX,
          "version request did not return the gate's no-tx verdict");
    CHECK(tx_calls == 0, "something transmitted with no transmit function");

    CHECK(r2_gate_op_name(0x13, 0x03) != NULL, "battery op is not in the gate's tables");
    CHECK(r2_gate_op_name(0x17, 0x14) != NULL, "head op is not in the gate's tables");
    CHECK(r2_gate_op_name(0x11, 0x00) != NULL, "version op is not in the gate's tables");

    /* And none of the three may be a FORBIDDEN entry the gate knows by name. */
    CHECK(r2_gate_check(0x13, 0x03) == R2_GATE_ALLOW, "battery is not allowed at READ");
    CHECK(r2_gate_check(0x17, 0x14) == R2_GATE_ALLOW, "head is not allowed at READ");
    CHECK(r2_gate_check(0x11, 0x00) == R2_GATE_ALLOW, "version is not allowed at READ");
}

/* The hard proof. Comparing return codes is not one -- a module that
 * reimplemented the gate's verdicts and encoded frames itself would return
 * identical codes, and a mutation shaped exactly like that SURVIVED the first
 * battery. It cannot, however, move the gate's own counter.
 *
 * The pair is what makes it airtight: the gate admitted exactly as many sends
 * as there were frames on the wire. One without the other still lets a bypass
 * through -- an extra unmetered send, or a metered call whose frame never
 * left. */
static void test_every_frame_was_admitted_by_the_gate(void)
{
    uint32_t admitted_before, admitted_after, refused_before, refused_after;
    r2_gate_stats(&admitted_before, &refused_before);
    tx_calls = 0;

    CHECK(r2_ops_request_battery(0x20, fake_tx, NULL) > 0, "battery refused");
    CHECK(r2_ops_request_head(0x21, fake_tx, NULL) > 0,    "head refused");
    CHECK(r2_ops_probe_version(0x22, fake_tx, NULL) > 0,   "version refused");

    r2_gate_stats(&admitted_after, &refused_after);
    CHECK(admitted_after - admitted_before == 3,
          "the gate admitted %u sends for 3 requests -- something bypassed it",
          admitted_after - admitted_before);
    CHECK(tx_calls == 3, "expected 3 frames on the wire, saw %d", tx_calls);
    CHECK(refused_after == refused_before,
          "a READ-tier op was refused at the default ceiling");

    /* A refusal must be counted as a refusal and must NOT reach the wire. */
    tx_calls = 0;
    r2_gate_stats(&admitted_before, &refused_before);
    CHECK(r2_gate_send(0x16, 0x07, 0x23, NULL, 0, fake_tx, NULL) == R2_GATE_FORBIDDEN,
          "drive was not forbidden");
    r2_gate_stats(&admitted_after, &refused_after);
    CHECK(admitted_after == admitted_before, "a forbidden op counted as admitted");
    CHECK(refused_after - refused_before == 1, "a forbidden op was not counted as refused");
    CHECK(tx_calls == 0, "a forbidden op reached the wire");
}

/* All three are READ tier, so all three are PERMITTED at the DEFAULT ceiling
 * with no operator action -- which is what makes them safe to run on connect.
 * "Permitted" is the whole claim. Whether R2 answers the version probe is
 * unknown and cannot be settled here. */
static void test_requests_are_permitted_at_the_default_ceiling(void)
{
    CHECK(r2_gate_get_ceiling() == R2_TIER_READ,
          "these tests assume the default ceiling is READ");

    tx_calls = 0;
    CHECK(r2_ops_request_battery(0x13, fake_tx, NULL) > 0, "battery request refused");
    CHECK(r2_ops_request_head(0x14, fake_tx, NULL) > 0,    "head request refused");
    CHECK(r2_ops_probe_version(0x15, fake_tx, NULL) > 0, "version request refused");
    CHECK(tx_calls == 3, "expected 3 transmits, got %d", tx_calls);
}

/* The exact bytes on the wire, so a did/cid typo cannot hide behind "the gate
 * allowed it". Hand-computed: checksum = 0xFF - (sum of flags..data & 0xFF). */
static void test_request_bytes(void)
{
    tx_calls = 0;
    (void)r2_ops_request_battery(0x13, fake_tx, NULL);
    /* 8D 0A 13 03 13 <sum> D8;  sum = 0xFF-(0x0A+0x13+0x03+0x13) = 0xCC */
    const uint8_t want[7] = { 0x8D, 0x0A, 0x13, 0x03, 0x13, 0xCC, 0xD8 };
    CHECK(tx_len == sizeof want, "battery request is %zu bytes, want %zu",
          tx_len, sizeof want);
    CHECK(tx_len == sizeof want && memcmp(tx_buf, want, sizeof want) == 0,
          "battery request bytes differ from the hand-computed frame");
}

/* ---- THE REAL FRAME ---------------------------------------------------- */

/* Captured from R2 during the #103 A1 endurance run, decoded end to end
 * through slice 1 and parsed by slice 3. This is the only piece of evidence in
 * this module that came off the droid rather than out of a spec. */
static void test_captured_battery_frame(void)
{
    const uint8_t raw[] = { 0x8D, 0x09, 0x13, 0x03, 0x13, 0x00,
                            0x01, 0xBA, 0x12, 0xD8 };
    uint8_t scratch[64];
    r2_response_t r;
    const int rc = r2_packet_decode(raw, sizeof raw, scratch, sizeof scratch, &r);
    CHECK(rc == R2_OK, "captured frame failed to decode: %d", rc);
    if (rc != R2_OK) return;

    r2_battery_t b;
    const r2_ops_err_t e = r2_ops_parse_battery(&r, &b);
    CHECK(e == R2_OPS_OK, "captured frame failed to parse: %s", r2_ops_err_name(e));
    CHECK(b.centivolts == 442, "want 442 cV, got %u", b.centivolts);
    CHECK(b.volts > 4.41f && b.volts < 4.43f, "want ~4.42 V, got %f", (double)b.volts);
}

/* Big-endian, and NOT accidentally little-endian: 0x4228 is 42.0 degrees,
 * whereas the byte-swapped reading is a denormal near zero. A dome angle is
 * the one number we act on, and D-013 measured that below ~10.5 deg of travel
 * he silently does not move -- an endianness slip would read every position as
 * "already there". */
static void test_head_float_and_endianness(void)
{
    const uint8_t deg42[4] = { 0x42, 0x28, 0x00, 0x00 };
    r2_head_t h;
    r2_response_t r = resp(0x17, 0x14, 0, deg42, 4);
    CHECK(r2_ops_parse_head(&r, &h) == R2_OPS_OK, "head parse failed");
    CHECK(h.degrees > 41.9f && h.degrees < 42.1f, "want 42.0, got %f", (double)h.degrees);

    /* Negative, because the dome turns both ways and -0.06 is a real reading
     * this project has recorded. */
    const uint8_t negative[4] = { 0xC2, 0x28, 0x00, 0x00 };  /* -42.0 */
    r = resp(0x17, 0x14, 0, negative, 4);
    CHECK(r2_ops_parse_head(&r, &h) == R2_OPS_OK, "negative head parse failed");
    CHECK(h.degrees < -41.9f && h.degrees > -42.1f, "want -42.0, got %f", (double)h.degrees);

    const uint8_t zero[4] = { 0, 0, 0, 0 };
    r = resp(0x17, 0x14, 0, zero, 4);
    CHECK(r2_ops_parse_head(&r, &h) == R2_OPS_OK, "zero head parse failed");
    CHECK(h.degrees == 0.0f, "want 0.0, got %f", (double)h.degrees);
}

static void test_version_fields_are_distinct(void)
{
    /* Distinct values in every field, so a copy-paste that reads the same two
     * bytes three times cannot pass. */
    const uint8_t payload[6] = { 0x00, 0x04, 0x00, 0x01, 0x02, 0x03 };
    r2_version_t v;
    r2_response_t r = resp(0x11, 0x00, 0, payload, 6);
    CHECK(r2_ops_parse_version(&r, &v) == R2_OPS_OK, "version parse failed");
    CHECK(v.major == 4,      "major: want 4, got %u", v.major);
    CHECK(v.minor == 1,      "minor: want 1, got %u", v.minor);
    CHECK(v.revision == 0x0203, "revision: want 515, got %u", v.revision);
}

static void test_battery_range(void)
{
    struct { uint8_t hi, lo; unsigned cv; } cases[] = {
        { 0x00, 0x00,    0 },     /* floor */
        { 0x01, 0xBA,  442 },     /* the observed reading */
        { 0x01, 0x2C,  300 },     /* a flat cell */
        { 0xFF, 0xFF, 65535 },    /* ceiling, proves no sign extension */
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        const uint8_t p[2] = { cases[i].hi, cases[i].lo };
        r2_response_t r = resp(0x13, 0x03, 0, p, 2);
        r2_battery_t b;
        CHECK(r2_ops_parse_battery(&r, &b) == R2_OPS_OK, "case %zu failed to parse", i);
        CHECK(b.centivolts == cases[i].cv, "case %zu: want %u cV, got %u",
              i, cases[i].cv, b.centivolts);
    }
}

int main(void)
{
    printf("r2_ops host tests\n");
    printf("  refusals\n");
    test_parser_refuses_another_ops_response();
    test_parser_refuses_a_device_error();
    test_parser_refuses_a_wrong_length();
    test_parser_refuses_nulls();
    printf("  routing\n");
    test_requests_route_through_the_gate();
    test_every_frame_was_admitted_by_the_gate();
    test_requests_are_permitted_at_the_default_ceiling();
    test_request_bytes();
    printf("  decoding\n");
    test_captured_battery_frame();
    test_head_float_and_endianness();
    test_version_fields_are_distinct();
    test_battery_range();

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
