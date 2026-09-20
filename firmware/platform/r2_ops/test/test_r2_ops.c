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
    CHECK(r2_gate_check(0x13, 0x03, NULL, 0) == R2_GATE_ALLOW, "battery is not allowed at READ");
    CHECK(r2_gate_check(0x17, 0x14, NULL, 0) == R2_GATE_ALLOW, "head is not allowed at READ");
    CHECK(r2_gate_check(0x11, 0x00, NULL, 0) == R2_GATE_ALLOW, "version is not allowed at READ");
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

/* ---- LEDs (#114 slice 5) ------------------------------------------------ */

/* THE test this project has been unable to write until now.
 *
 * Slice 2 shipped a permission ceiling and slices 3-4 gave it nothing to
 * refuse: every op was READ tier, so the ceiling was exercised only against
 * hypothetical did/cid pairs in the gate's own tests. An LED write is the
 * first REAL op that sits above the default, and this is the first time the
 * ladder has been asked to stop something a caller genuinely wanted to do.
 *
 * It runs before any test raises the ceiling, on purpose. */
static void test_leds_are_refused_at_the_default_ceiling(void)
{
    CHECK(r2_gate_get_ceiling() == R2_TIER_READ,
          "this test is void unless the ceiling is still at its default");

    const uint8_t values[6] = { 255, 0, 0, 255, 0, 0 };
    tx_calls = 0;
    uint32_t admitted_before, refused_before, admitted_after, refused_after;
    r2_gate_stats(&admitted_before, &refused_before);

    CHECK(r2_ops_set_rgb(255, 0, 0, 0x40, fake_tx, NULL) == R2_GATE_ABOVE_CEILING,
          "set_rgb was not refused at the READ ceiling");
    CHECK(r2_ops_leds_off(0x41, fake_tx, NULL) == R2_GATE_ABOVE_CEILING,
          "leds_off was not refused at the READ ceiling");
    CHECK(r2_ops_set_leds(R2_LED_MASK_FRONT, values, 3, 0x42, fake_tx, NULL)
              == R2_GATE_ABOVE_CEILING,
          "set_leds was not refused at the READ ceiling");

    /* Refusal means NOTHING WENT OUT. A verdict returned while the frame still
     * reached the radio would be a log line, not a gate. */
    CHECK(tx_calls == 0, "%d frames reached the wire from a refused LED write",
          tx_calls);
    r2_gate_stats(&admitted_after, &refused_after);
    CHECK(admitted_after == admitted_before, "a refused LED write counted as admitted");
    CHECK(refused_after - refused_before == 3, "3 refusals expected, gate counted %u",
          refused_after - refused_before);
}

/* A value count that disagrees with the mask is the one way to build a packet
 * that is well-formed and means something else: nothing in the payload states
 * how many values follow, so R2 reads whatever is there. Refused before the
 * gate, because it is malformed rather than forbidden. */
static void test_leds_refuse_a_mask_and_value_mismatch(void)
{
    const uint8_t v[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    tx_calls = 0;

    CHECK(r2_ops_set_leds(R2_LED_MASK_FRONT, v, 2, 0x50, fake_tx, NULL)
              == R2_OPS_BAD_LED_REQUEST, "3-bit mask accepted 2 values");
    CHECK(r2_ops_set_leds(R2_LED_MASK_FRONT, v, 4, 0x51, fake_tx, NULL)
              == R2_OPS_BAD_LED_REQUEST, "3-bit mask accepted 4 values");
    CHECK(r2_ops_set_leds(R2_LED_MASK_ALL, v, 7, 0x52, fake_tx, NULL)
              == R2_OPS_BAD_LED_REQUEST, "8-bit mask accepted 7 values");
    CHECK(r2_ops_set_leds(1u << R2_LED_LOGIC, v, 0, 0x53, fake_tx, NULL)
              == R2_OPS_BAD_LED_REQUEST, "1-bit mask accepted 0 values");

    CHECK(r2_ops_set_leds(0, v, 0, 0x54, fake_tx, NULL) == R2_OPS_BAD_LED_REQUEST,
          "an empty mask was accepted");
    CHECK(r2_ops_set_leds(R2_LED_MASK_FRONT, NULL, 3, 0x55, fake_tx, NULL)
              == R2_OPS_BAD_LED_REQUEST, "a null value array was accepted");

    /* Bits above 7 are not channels on this droid. An unmapped bit is a guess
     * about hardware, sent as a command. */
    CHECK(r2_ops_set_leds(0x0100u, v, 1, 0x56, fake_tx, NULL) == R2_OPS_BAD_LED_REQUEST,
          "bit 8 was accepted as a channel");
    CHECK(r2_ops_set_leds(0x8000u, v, 1, 0x57, fake_tx, NULL) == R2_OPS_BAD_LED_REQUEST,
          "bit 15 was accepted as a channel");
    CHECK(r2_ops_set_leds(0x01FFu, v, 8, 0x58, fake_tx, NULL) == R2_OPS_BAD_LED_REQUEST,
          "a mask straddling bit 8 was accepted");

    CHECK(tx_calls == 0, "a malformed LED write reached the wire");

    /* And a malformed request must be rejected BEFORE the gate, so it cannot
     * even be counted as something we tried to send. */
    uint32_t a, r;
    r2_gate_stats(&a, &r);
    CHECK(r2_ops_set_leds(0, v, 0, 0x59, fake_tx, NULL) == R2_OPS_BAD_LED_REQUEST,
          "empty mask");
    uint32_t a2, r2_;
    r2_gate_stats(&a2, &r2_);
    CHECK(a2 == a && r2_ == r, "a malformed request reached the gate's counters");
}

/* Raising the ceiling is the operator's decision. Once raised, the exact bytes
 * matter: values are ordered by ASCENDING BIT, and getting that wrong swaps
 * red for blue with no error anywhere. */
/* The status path (D-030). The ceiling is raised to the TOP here on purpose:
 * if r2_ops_status_leds went through r2_gate_send, the ceiling would admit
 * it and the ungranted refusal below would fail. */
static void test_status_leds_need_the_grant_not_the_ceiling(void)
{
    const uint8_t v[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    r2_gate_set_ceiling(R2_TIER_STANCE);
    r2_gate_grant_status(false);
    tx_calls = 0;
    CHECK(r2_ops_status_leds(v, 0x40, fake_tx, NULL) == R2_GATE_NOT_GRANTED && tx_calls == 0,
          "an ungranted status light went out at the top ceiling (%d frames)", tx_calls);
    CHECK(r2_ops_status_leds(NULL, 0x40, fake_tx, NULL) == R2_OPS_BAD_LED_REQUEST,
          "NULL values must be refused as malformed");

    r2_gate_set_ceiling(R2_TIER_READ);
    r2_gate_grant_status(true);
    uint32_t a0, r0, a1, r1;
    r2_gate_stats(&a0, &r0);
    tx_calls = 0;
    CHECK(r2_ops_status_leds(v, 0x41, fake_tx, NULL) > 0 && tx_calls == 1,
          "a granted status light was refused at READ");
    r2_gate_stats(&a1, &r1);
    CHECK(a1 == a0 + 1, "the status light did not pass through the gate");
    /* 8D 0A 1A 0E 41 00 FF 01..08 <chk> D8: every channel, in bit order */
    const uint8_t want_body[] = { 0x0A, 0x1A, 0x0E, 0x41, 0x00, 0xFF,
                                  1, 2, 3, 4, 5, 6, 7, 8 };
    CHECK(tx_len == sizeof want_body + 3 && tx_buf[0] == 0x8D &&
          memcmp(tx_buf + 1, want_body, sizeof want_body) == 0,
          "status frame body differs from the hand-computed bytes");
    r2_gate_grant_status(false);
}

static void test_leds_once_the_operator_raises_the_ceiling(void)
{
    r2_gate_set_ceiling(R2_TIER_LEDS);

    tx_calls = 0;
    CHECK(r2_ops_set_rgb(0x11, 0x22, 0x33, 0x60, fake_tx, NULL) > 0,
          "set_rgb refused at the LEDS ceiling");
    CHECK(tx_calls == 1, "set_rgb sent %d frames", tx_calls);

    /* 8D 0A 1A 0E 60 00 77 11 22 33 11 22 33 <chk> D8
     * mask 0x0077 is front RGB (0,1,2) + back RGB (4,5,6); six values follow in
     * ascending bit order, so front then back. */
    const uint8_t want_body[] = { 0x0A, 0x1A, 0x0E, 0x60,
                                  0x00, 0x77, 0x11, 0x22, 0x33, 0x11, 0x22, 0x33 };
    CHECK(tx_len == sizeof want_body + 3, "frame is %zu bytes, want %zu",
          tx_len, sizeof want_body + 3);
    CHECK(tx_buf[0] == 0x8D, "no SOP");
    CHECK(memcmp(tx_buf + 1, want_body, sizeof want_body) == 0,
          "LED frame body differs from the hand-computed bytes");
    /* checksum = 0xFF - (sum of body & 0xFF) */
    unsigned sum = 0;
    for (size_t i = 0; i < sizeof want_body; i++) sum += want_body[i];
    CHECK(tx_buf[1 + sizeof want_body] == (uint8_t)(0xFF - (sum & 0xFFu)),
          "checksum wrong");
    CHECK(tx_buf[tx_len - 1] == 0xD8, "no EOP");

    /* leds_off must touch every channel, including logic and holo -- a
     * teardown that leaves two fixtures lit is not a teardown. */
    tx_calls = 0;
    CHECK(r2_ops_leds_off(0x61, fake_tx, NULL) > 0, "leds_off refused");
    CHECK(tx_buf[5] == 0x00 && tx_buf[6] == 0xFF, "leds_off mask is not 0x00FF");
    for (int i = 0; i < 8; i++)
        CHECK(tx_buf[7 + i] == 0x00, "leds_off value %d is not zero", i);

    /* A single-channel write: logic on. One bit, one value. */
    const uint8_t on[1] = { 255 };
    tx_calls = 0;
    CHECK(r2_ops_set_leds(1u << R2_LED_LOGIC, on, 1, 0x62, fake_tx, NULL) > 0,
          "single-channel write refused");
    CHECK(tx_buf[5] == 0x00 && tx_buf[6] == 0x08, "logic mask is not 0x0008");
    CHECK(tx_buf[7] == 255, "logic value not carried");

    /* Escaping still applies: 0xAB as a colour value must be escaped, and a
     * hand-built encoder would have shipped it raw. D-024 exists because that
     * went unnoticed for a whole endurance run. */
    const uint8_t esc[3] = { 0x8D, 0xAB, 0xD8 };
    tx_calls = 0;
    CHECK(r2_ops_set_leds(R2_LED_MASK_FRONT, esc, 3, 0x63, fake_tx, NULL) > 0,
          "a colour containing framing bytes was refused");
    for (size_t i = 1; i + 1 < tx_len; i++)
        CHECK(!(tx_buf[i] == 0x8D || tx_buf[i] == 0xD8),
              "raw framing byte at offset %zu -- not escaped", i);

    /* Put it back, so no later test inherits a raised ceiling. */
    r2_gate_set_ceiling(R2_TIER_READ);
}

/* ---- Reply chirp (D-032, step 3.5a) -------------------------------------- */

static uint32_t next_test_exchange_id(void)
{
    /* Ever-increasing, matching panel_exchange's own invariant -- so each
     * test gets a fresh id with no chirp spent on it yet, no reset needed. */
    static uint32_t id = 5000;
    return ++id;
}

/* THE ILLEGAL CASE this table exists for: a mood the model was never allowed
 * to send must not silently pick a sound. */
static void test_reply_chirp_refuses_no_mood(void)
{
    printf("  reply chirp\n");
    CHECK(r2_ops_chirp_id_for_mood(VOICE_MOOD_NONE) == 0,
          "VOICE_MOOD_NONE has a chirp id");
    r2_gate_grant_status(true);
    const uint32_t id = next_test_exchange_id();
    tx_calls = 0;
    const int n = r2_ops_reply_chirp(VOICE_MOOD_NONE, true, id, 1, fake_tx, NULL);
    CHECK(n == R2_GATE_REPLY_BAD_ID && tx_calls == 0,
          "a NONE-mood reply chirp went out (got %d, tx %d)", n, tx_calls);
    r2_gate_grant_status(false);
}

/* An enum value outside voice_mood_t entirely -- not just NONE, a value the
 * type was never supposed to hold. chirp_id_for_mood()'s switch has a
 * `default: return 0` for exactly this, but nothing exercised that arm until
 * now; a refactor that turned the switch exhaustive-without-default (and so
 * silently returned garbage for an out-of-range value under -Wswitch) would
 * have shipped with every other test here still green. */
static void test_reply_chirp_refuses_an_out_of_range_mood(void)
{
    const voice_mood_t bogus = (voice_mood_t)99;
    CHECK(r2_ops_chirp_id_for_mood(bogus) == 0, "an out-of-range mood has a chirp id");
    r2_gate_grant_status(true);
    const uint32_t id = next_test_exchange_id();
    tx_calls = 0;
    const int n = r2_ops_reply_chirp(bogus, true, id, 1, fake_tx, NULL);
    CHECK(n == R2_GATE_REPLY_BAD_ID && tx_calls == 0,
          "an out-of-range mood reply chirp went out (got %d, tx %d)", n, tx_calls);
    r2_gate_grant_status(false);
}

/* PIN THE EXACT id per mood, not just "some id the gate admits". A mood
 * mapped to a DIFFERENT mood's (still gate-admitted) id passed the "does the
 * gate accept it" test below and reads as a working reply while playing the
 * wrong chirp for what he heard -- a CX bug no refusal-shaped test can catch. */
static void test_each_mood_picks_its_own_committed_id(void)
{
    CHECK(r2_ops_chirp_id_for_mood(VOICE_MOOD_CURIOUS) == 1966, "curious != R2_CHATTY_11");
    CHECK(r2_ops_chirp_id_for_mood(VOICE_MOOD_HAPPY)   == 3302, "happy != R2_POSITIVE_1");
    CHECK(r2_ops_chirp_id_for_mood(VOICE_MOOD_ANNOYED) == 1910, "annoyed != R2_ANNOYED");
    CHECK(r2_ops_chirp_id_for_mood(VOICE_MOOD_SAD)     == 3484, "sad != R2_SAD_1");
    CHECK(r2_ops_chirp_id_for_mood(VOICE_MOOD_ALERT)   == 1737, "alert != R2_ALARM_1");
}

/* Every mood in the table must pick an id the GATE actually admits -- the two
 * tables are maintained separately (r2_ops.c's mood table, r2_gate.c's
 * REPLY_CHIRP_IDS), so this is the test that would catch them drifting apart.
 * A mood whose id r2_ops picks but r2_gate refuses is a reply that silently
 * never chirps. */
static void test_every_mood_picks_an_id_the_gate_admits(void)
{
    static const voice_mood_t moods[] = {
        VOICE_MOOD_CURIOUS, VOICE_MOOD_HAPPY, VOICE_MOOD_ANNOYED,
        VOICE_MOOD_SAD, VOICE_MOOD_ALERT,
    };
    r2_gate_grant_status(true);
    for (size_t i = 0; i < sizeof moods / sizeof moods[0]; i++) {
        const uint16_t id = r2_ops_chirp_id_for_mood(moods[i]);
        CHECK(id != 0, "mood %s has no chirp id", voice_mood_name(moods[i]));

        const uint32_t exch = next_test_exchange_id();
        tx_calls = 0;
        const int n = r2_ops_reply_chirp(moods[i], true, exch, 1, fake_tx, NULL);
        CHECK(n > 0 && tx_calls == 1,
              "mood %s's chirp id %u was refused by the gate (%d)",
              voice_mood_name(moods[i]), id, n);
    }
    r2_gate_grant_status(false);
}

static void test_reply_chirp_needs_may_act_and_the_grant(void)
{
    printf("  reply chirp refuses without may_act or the grant\n");
    const uint32_t id1 = next_test_exchange_id();
    r2_gate_grant_status(false);
    tx_calls = 0;
    int n = r2_ops_reply_chirp(VOICE_MOOD_HAPPY, true, id1, 1, fake_tx, NULL);
    CHECK(n == R2_GATE_NOT_GRANTED && tx_calls == 0,
          "an ungranted happy chirp went out (got %d, tx %d)", n, tx_calls);

    const uint32_t id2 = next_test_exchange_id();
    r2_gate_grant_status(true);
    tx_calls = 0;
    n = r2_ops_reply_chirp(VOICE_MOOD_HAPPY, false, id2, 1, fake_tx, NULL);
    CHECK(n == R2_GATE_REPLY_NOT_LIVE && tx_calls == 0,
          "a happy chirp with may_act=false went out (got %d, tx %d)", n, tx_calls);
    r2_gate_grant_status(false);
}

/* THE HARD PROOF, the same shape as test_every_frame_was_admitted_by_the_gate
 * above: r2_ops_reply_chirp must be routing through the gate's real reply
 * door, not reimplementing its verdicts, so the gate's own admitted counter
 * and the one-per-exchange rule must both be provably live from here. */
static void test_reply_chirp_routes_through_the_gate_and_its_budget(void)
{
    r2_gate_grant_status(true);
    const uint32_t id = next_test_exchange_id();
    uint32_t a0, r0, a1, r1;
    r2_gate_stats(&a0, &r0);
    tx_calls = 0;
    CHECK(r2_ops_reply_chirp(VOICE_MOOD_SAD, true, id, 1, fake_tx, NULL) > 0,
          "sad chirp refused");
    r2_gate_stats(&a1, &r1);
    CHECK(a1 == a0 + 1 && tx_calls == 1,
          "the reply chirp did not move the gate's admitted counter by one");

    /* Same exchange, second mood: still refused, because the BUDGET is keyed
     * on the exchange id, not on which mood asked. */
    tx_calls = 0;
    const int n = r2_ops_reply_chirp(VOICE_MOOD_ALERT, true, id, 2, fake_tx, NULL);
    CHECK(n == R2_GATE_REPLY_USED && tx_calls == 0,
          "a second mood for the same exchange still got a chirp (%d)", n);
    r2_gate_grant_status(false);
}

/* ---- #168 part 3: the stop --------------------------------------------- */

static uint8_t stop_seqs[8];
static unsigned stop_seq_n;
static uint8_t stop_seq_next(void)
{
    const uint8_t v = (uint8_t)(100u + stop_seq_n);
    if (stop_seq_n < sizeof stop_seqs / sizeof stop_seqs[0])
        stop_seqs[stop_seq_n] = v;
    stop_seq_n++;
    return v;
}

static unsigned stop_tx_calls;
static int stop_tx(const uint8_t *f, size_t n, void *ctx)
{
    (void)f; (void)n; (void)ctx;
    stop_tx_calls++;
    return 0;
}

/* A transport that FAILS. A send whose tx returns < 0 must be reported as not
 * sent -- but must not stop the other two being attempted. */
static int stop_tx_broken(const uint8_t *f, size_t n, void *ctx)
{
    (void)f; (void)n; (void)ctx;
    stop_tx_calls++;
    return -1;
}

/* Fails call N only. A transport that fails ALL or NONE never exercises the
 * report's arithmetic -- `r.sent = r.animation ? 3u : 0u` survived a suite
 * that had both. The subset is the only regime `sent` exists for. */
static unsigned stop_tx_fail_on;
static int stop_tx_flaky(const uint8_t *f, size_t n, void *ctx)
{
    (void)f; (void)n; (void)ctx;
    stop_tx_calls++;
    return (stop_tx_calls == stop_tx_fail_on) ? -1 : 0;
}

static void test_the_count_matches_which_halts_got_out(void)
{
    /* Fail exactly one of the three, each in turn, and assert the report names
     * the right one. This is the field the button prints as "%u/3 SENT"; a
     * count that rounds up tells the operator he has stopped when he has not. */
    r2_gate_set_ceiling(R2_TIER_READ);
    for (unsigned bad = 1u; bad <= 3u; bad++) {
        stop_seq_n = 0; stop_tx_calls = 0; stop_tx_fail_on = bad;
        const r2_stop_report_t r =
            r2_ops_stop_all(stop_seq_next, stop_tx_flaky, NULL);

        CHECK(r.sent == 2u, "one send failed but the report says %u of 3", r.sent);
        CHECK(!r2_stop_is_complete(r), "two of three reported as complete");
        /* All three still ATTEMPTED: a failure must not short-circuit. */
        CHECK(stop_tx_calls == 3u, "only %u of 3 attempted when #%u failed",
              stop_tx_calls, bad);

        /* And the flag that is false is the one that failed. Order is
         * animation, audio, legs. */
        CHECK(r.animation == (bad != 1u), "animation flag wrong for bad=%u", bad);
        CHECK(r.audio     == (bad != 2u), "audio flag wrong for bad=%u", bad);
        CHECK(r.legs      == (bad != 3u), "legs flag wrong for bad=%u", bad);
    }
}

static void test_a_partial_stop_never_reports_complete(void)
{
    /* THE ILLEGAL CASE. "A rejected stop and a successful one were
     * indistinguishable, on the path where nobody is watching" -- the
     * prototype's own note. A report that rounds up is worse than no report:
     * it tells the operator he has stopped when he has not. */
    r2_stop_report_t r = { true, true, false, 2u };
    CHECK(!r2_stop_is_complete(r), "two of three reported as a complete stop");
    r = (r2_stop_report_t){ true, false, true, 2u };
    CHECK(!r2_stop_is_complete(r), "a missing audio halt reported complete");
    r = (r2_stop_report_t){ false, true, true, 2u };
    CHECK(!r2_stop_is_complete(r), "a missing animation halt reported complete");
    r = (r2_stop_report_t){ false, false, false, 0u };
    CHECK(!r2_stop_is_complete(r), "a stop that sent nothing reported complete");
    r = (r2_stop_report_t){ true, true, true, 3u };
    CHECK(r2_stop_is_complete(r), "all three sent was not reported complete");
}

static void test_the_stop_fires_all_three_at_a_read_ceiling(void)
{
    /* The panel's ceiling. All three are HALTS (D-026), so all three go. */
    r2_gate_set_ceiling(R2_TIER_READ);
    stop_seq_n = 0; stop_tx_calls = 0;
    const r2_stop_report_t r = r2_ops_stop_all(stop_seq_next, stop_tx, NULL);

    CHECK(r.animation, "the animation halt did not go");
    CHECK(r.audio, "the audio halt did not go at a READ ceiling");
    CHECK(r.legs, "the legs halt did not go -- it is the one that was banned");
    CHECK(r.sent == 3u, "sent %u of 3", r.sent);
    CHECK(r2_stop_is_complete(r), "a full stop did not report complete");
    CHECK(stop_tx_calls == 3u, "%u frames reached the transport", stop_tx_calls);

    /* THREE DISTINCT SEQS. One seq across three sends lets a single reply
     * resolve all of them. */
    CHECK(stop_seq_n == 3u, "next_seq was called %u times", stop_seq_n);
    CHECK(stop_seqs[0] != stop_seqs[1] && stop_seqs[1] != stop_seqs[2] &&
          stop_seqs[0] != stop_seqs[2], "the three halts shared a seq");
}

static void test_one_failing_send_does_not_skip_the_others(void)
{
    /* The prototype's scar: building the sends up front meant a failure at
     * call time skipped the rest, so "always attempts all" was false exactly
     * when it mattered. Every one must still be ATTEMPTED. */
    r2_gate_set_ceiling(R2_TIER_READ);
    stop_seq_n = 0; stop_tx_calls = 0;
    const r2_stop_report_t r = r2_ops_stop_all(stop_seq_next, stop_tx_broken, NULL);

    CHECK(stop_tx_calls == 3u,
          "a failing transport stopped the stop after %u of 3", stop_tx_calls);
    CHECK(stop_seq_n == 3u, "only %u of 3 halts were attempted", stop_seq_n);
    CHECK(!r2_stop_is_complete(r), "a stop whose sends all failed read complete");

    /* `sent` IS WHAT THE BUTTON SHOWS. A mutation that set it to 3
     * unconditionally survived a suite that only ever read it on the happy
     * path -- which is the prototype's "a rejected stop and a successful one
     * were indistinguishable", reintroduced in the one field the operator
     * reads. Every flag false means every count zero. */
    CHECK(r.sent == 0u, "nothing sent, but the report says %u", r.sent);
    CHECK(!r.animation && !r.audio && !r.legs,
          "a failing transport still reported a halt away");
}

static void test_a_stop_with_no_transport_sends_nothing_and_says_so(void)
{
    stop_seq_n = 0; stop_tx_calls = 0;
    r2_stop_report_t r = r2_ops_stop_all(stop_seq_next, NULL, NULL);
    CHECK(r.sent == 0u, "a stop with no transport reported %u sent", r.sent);
    CHECK(!r2_stop_is_complete(r), "a stop with no transport reported complete");
    CHECK(stop_tx_calls == 0u, "something transmitted without a transport");
    /* AND IT BURNS NO SEQUENCE NUMBERS. Without the early return the gate
     * still refuses each send, so the report is right by accident while three
     * seqs are consumed for nothing -- and a seq issued but never used is one
     * a later reply can alias onto. The guard is what makes it deliberate. */
    CHECK(stop_seq_n == 0u,
          "a stop with no transport still drew %u sequence numbers", stop_seq_n);

    r = r2_ops_stop_all(NULL, stop_tx, NULL);
    CHECK(r.sent == 0u, "a stop with no seq source reported %u sent", r.sent);
    CHECK(stop_tx_calls == 0u, "a stop with no seq source still transmitted");
}

int main(void)
{
    test_a_partial_stop_never_reports_complete();
    test_the_count_matches_which_halts_got_out();
    test_the_stop_fires_all_three_at_a_read_ceiling();
    test_one_failing_send_does_not_skip_the_others();
    test_a_stop_with_no_transport_sends_nothing_and_says_so();

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
    printf("  leds\n");
    test_leds_are_refused_at_the_default_ceiling();
    test_leds_refuse_a_mask_and_value_mismatch();
    test_leds_once_the_operator_raises_the_ceiling();
    test_status_leds_need_the_grant_not_the_ceiling();
    printf("  decoding\n");
    test_captured_battery_frame();
    test_head_float_and_endianness();
    test_version_fields_are_distinct();
    test_battery_range();

    test_reply_chirp_refuses_no_mood();
    test_reply_chirp_refuses_an_out_of_range_mood();
    test_each_mood_picks_its_own_committed_id();
    test_every_mood_picks_an_id_the_gate_admits();
    test_reply_chirp_needs_may_act_and_the_grant();
    test_reply_chirp_routes_through_the_gate_and_its_budget();

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
