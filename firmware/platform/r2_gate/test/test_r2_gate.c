/* Host tests for the permission ceiling (#114 slice 2, AC4).
 *
 * The ILLEGAL cases come first and the legal ones after, deliberately. This
 * repo has three mutations on record that survived because the tests asserted
 * the shipped table was valid rather than that the guard rejects bad input --
 * "removing the status-colour guard passed, because every state in the table
 * used a legal colour" (CLAUDE.md). So every test below that matters is a
 * REFUSAL.
 */
#include <stdio.h>
#include <string.h>

#include "r2_gate.h"
/* For the frame the gate SHOULD have produced: the wire-bytes assertion
 * builds the expected encoding rather than trusting a call count. */
#include "r2_packet.h"

static int failures = 0, checks = 0;

/* Trips the moment any test sets a ceiling. test_untouched_default() asserts it
 * is still clear, so that test cannot silently become a test of the setter. */
static int s_ceiling_touched = 0;

#define CHECK(cond, ...) do {                                \
    checks++;                                                \
    if (!(cond)) { failures++;                               \
        printf("  FAIL: "); printf(__VA_ARGS__);             \
        printf("\n        at %s:%d\n", __FILE__, __LINE__); }\
} while (0)

/* A transmit hook that records whether anything actually left. */
static int   tx_calls;
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
static void tx_reset(void) { tx_calls = 0; tx_len = 0; }

/* The six ops that must never be sendable, whatever the ceiling. */
static const struct { uint8_t did, cid; const char *name; } FORBIDDEN[] = {
    { 0x13, 0x01, "sleep" },
    { 0x13, 0x00, "enter_deep_sleep" },
    { 0x17, 0x05, "play_animation" },
    { 0x17, 0x0D, "perform_leg_action" },
    { 0x17, 0x15, "set_leg_position" },
    { 0x16, 0x07, "drive" },
};
#define FORBIDDEN_N (sizeof FORBIDDEN / sizeof FORBIDDEN[0])

/* ---- ILLEGAL FIRST ------------------------------------------------------- */

static void test_forbidden_at_every_ceiling(void)
{
    printf("AC4  forbidden ops are refused at EVERY ceiling, and send nothing\n");
    for (int c = 0; c < R2_TIER__COUNT; c++) {
        s_ceiling_touched = 1; r2_gate_set_ceiling((r2_tier_t)c);
        for (size_t i = 0; i < FORBIDDEN_N; i++) {
            CHECK(r2_gate_check(FORBIDDEN[i].did, FORBIDDEN[i].cid, NULL, 0) == R2_GATE_FORBIDDEN,
                  "%s must be FORBIDDEN at ceiling %s",
                  FORBIDDEN[i].name, r2_gate_tier_name((r2_tier_t)c));
            tx_reset();
            int rc = r2_gate_send(FORBIDDEN[i].did, FORBIDDEN[i].cid, 0,
                                  NULL, 0, fake_tx, NULL);
            CHECK(rc == R2_GATE_FORBIDDEN, "%s send must be refused", FORBIDDEN[i].name);
            /* The refusal is worthless if bytes still went out. */
            CHECK(tx_calls == 0, "%s: %d bytes were TRANSMITTED despite refusal",
                  FORBIDDEN[i].name, tx_calls);
        }
    }
}

static void test_unknown_ops_refused(void)
{
    printf("     unknown ops are refused by default (allowlist, not denylist)\n");
    s_ceiling_touched = 1; r2_gate_set_ceiling(R2_TIER_STANCE);           /* highest, to isolate the cause */
    int refused = 0, total = 0;
    for (unsigned did = 0; did < 0x40; did++) {
        for (unsigned cid = 0; cid < 0x40; cid++) {
            r2_gate_verdict_t v = r2_gate_check((uint8_t)did, (uint8_t)cid,
                                               NULL, 0);
            total++;
            if (v == R2_GATE_NOT_ALLOWLISTED) refused++;
            else CHECK(v == R2_GATE_ALLOW || v == R2_GATE_FORBIDDEN,
                       "did=0x%02X cid=0x%02X gave an unexpected verdict %d", did, cid, v);
        }
    }
    CHECK(refused > total - 40, "most of the op space must be unlisted (%d/%d)",
          refused, total);
    tx_reset();
    CHECK(r2_gate_send(0x99, 0x99, 0, NULL, 0, fake_tx, NULL) == R2_GATE_NOT_ALLOWLISTED,
          "an invented op must be refused");
    CHECK(tx_calls == 0, "an invented op must transmit nothing");
}

static void test_above_ceiling_refused(void)
{
    printf("     every op above the ceiling is refused, and sends nothing\n");
    /* At the lowest rung, everything that is not read-tier must be refused. */
    s_ceiling_touched = 1; r2_gate_set_ceiling(R2_TIER_READ);
    const struct { uint8_t did, cid; const char *name; } higher[] = {
        { 0x1A, 0x0E, "set_leds_16bit" },
        { 0x1A, 0x07, "play_audio" },
        { 0x1A, 0x08, "set_volume" },
        /* stop_audio WAS here. Its removal is the RULE changing, not a test
         * being weakened to fit the code (#168 part 3): it is a HALT, and a
         * halt is admitted at every ceiling, because a stop you have to raise
         * a ceiling to reach is not a stop -- and the panel's ceiling is READ.
         *
         * The ladder property this test exists for is untouched and still
         * asserted by every remaining row. play_audio and set_volume, the two
         * AUDIO-tier ops that make him DO something, are still refused here;
         * only the one that makes him stop moved. Its new behaviour is pinned
         * in test_every_halt_is_admitted_at_every_ceiling. */
        { 0x17, 0x0F, "set_head_position" },
    };
    for (size_t i = 0; i < sizeof higher / sizeof higher[0]; i++) {
        CHECK(r2_gate_check(higher[i].did, higher[i].cid, NULL, 0) == R2_GATE_ABOVE_CEILING,
              "%s must be above a read ceiling", higher[i].name);
        tx_reset();
        int rc = r2_gate_send(higher[i].did, higher[i].cid, 0, NULL, 0, fake_tx, NULL);
        CHECK(rc == R2_GATE_ABOVE_CEILING, "%s send must be refused", higher[i].name);
        CHECK(tx_calls == 0, "%s transmitted despite being above the ceiling",
              higher[i].name);
    }
    /* LEDs allowed at leds, audio still refused: the ladder is a ladder. */
    s_ceiling_touched = 1; r2_gate_set_ceiling(R2_TIER_LEDS);
    CHECK(r2_gate_check(0x1A, 0x0E, NULL, 0) == R2_GATE_ALLOW, "leds allowed at the leds rung");
    CHECK(r2_gate_check(0x1A, 0x07, NULL, 0) == R2_GATE_ABOVE_CEILING, "audio still refused at leds");
    CHECK(r2_gate_check(0x17, 0x0F, NULL, 0) == R2_GATE_ABOVE_CEILING, "dome still refused at leds");
}

/* MUST RUN FIRST, and the ordering is load-bearing rather than stylistic.
 *
 * A mutation that changed the initialiser to the HIGHEST rung survived the whole
 * suite, because the original version of this test called set_ceiling(READ)
 * before asserting -- with a comment claiming that proved "the value rather than
 * the residue". It proved the setter. The default is a safety property and the
 * only moment it is observable is before anything sets it. */
static void test_untouched_default(void)
{
    printf("     the UNTOUCHED default ceiling is the lowest rung\n");
    CHECK(s_ceiling_touched == 0,
          "this test must run before anything calls set_ceiling, or it proves nothing");
    CHECK(r2_gate_get_ceiling() == R2_TIER_READ,
          "a firmware that never sets a ceiling must default to read");
    CHECK(r2_gate_check(0x1A, 0x0E, NULL, 0) == R2_GATE_ABOVE_CEILING,
          "with no ceiling set, an LED write must already be refused");
    CHECK(r2_gate_check(0x17, 0x0F, NULL, 0) == R2_GATE_ABOVE_CEILING,
          "with no ceiling set, a dome move must already be refused");
    CHECK(!r2_gate_status_granted(),
          "a firmware that never grants must not hold the status grant");
    const uint8_t blue[] = {0x00, 0xFF, 0, 0, 255, 0, 0, 0, 255, 0};
    tx_reset();
    CHECK(r2_gate_send_status(0x1A, 0x0E, 1, blue, sizeof blue, fake_tx, NULL)
              == R2_GATE_NOT_GRANTED && tx_calls == 0,
          "with no grant, the status path must refuse a light and send nothing");
}

static void test_clamping(void)
{
    printf("     an out-of-range ceiling clamps DOWN, never up\n");
    s_ceiling_touched = 1; r2_gate_set_ceiling((r2_tier_t)99);
    CHECK(r2_gate_get_ceiling() == R2_TIER_READ,
          "an out-of-range ceiling must clamp DOWN, never up");
    CHECK(r2_gate_check(0x17, 0x0F, NULL, 0) == R2_GATE_ABOVE_CEILING,
          "a bad ceiling value must not become a privilege escalation");
}

/* ---- and only then, the legal path --------------------------------------- */

static void test_allowed_ops_transmit(void)
{
    printf("     allowed ops DO transmit, with the bytes the packet layer makes\n");
    s_ceiling_touched = 1; r2_gate_set_ceiling(R2_TIER_READ);
    tx_reset();
    int n = r2_gate_send(0x13, 0x0D, 0x02, NULL, 0, fake_tx, NULL);   /* wake */
    CHECK(n == 7, "wake encodes to 7 bytes (got %d)", n);
    CHECK(tx_calls == 1, "wake must transmit exactly once (got %d)", tx_calls);
    const uint8_t want[] = {0x8D, 0x0A, 0x13, 0x0D, 0x02, 0xD3, 0xD8};
    CHECK(tx_len == sizeof want && memcmp(tx_buf, want, sizeof want) == 0,
          "wake bytes must match the packet layer's output");

    /* seq 72's checksum is 0x8D and must arrive escaped even through the gate. */
    tx_reset();
    n = r2_gate_send(0x13, 0x0D, 72, NULL, 0, fake_tx, NULL);
    CHECK(n == 8 && tx_buf[5] == 0xAB && tx_buf[6] == 0x05,
          "the gate must not bypass escaping (len %d)", n);

    s_ceiling_touched = 1; r2_gate_set_ceiling(R2_TIER_LEDS);
    tx_reset();
    const uint8_t leds[] = {0x00, 0xFF, 0x8D, 0xD8};
    n = r2_gate_send(0x1A, 0x0E, 0x03, leds, sizeof leds, fake_tx, NULL);
    CHECK(n > 0 && tx_calls == 1, "an led write at the leds rung must go out");
}

static void test_no_tx_is_refused_not_crashed(void)
{
    printf("     a missing transmit hook is refused, not dereferenced\n");
    s_ceiling_touched = 1; r2_gate_set_ceiling(R2_TIER_READ);
    CHECK(r2_gate_send(0x13, 0x0D, 0, NULL, 0, NULL, NULL) == R2_GATE_NO_TX,
          "send with no tx must return NO_TX");
    /* And a refused op with no tx must still refuse for the RIGHT reason. */
    CHECK(r2_gate_send(0x13, 0x01, 0, NULL, 0, NULL, NULL) == R2_GATE_FORBIDDEN,
          "forbidden must beat the missing-tx error: the gate decides first");
}

/* ---- #168 part 3: a halt is admissible at every tier -------------------- */

static void test_a_leg_action_that_is_not_stop_is_still_forbidden(void)
{
    /* THE ILLEGAL CASE, and the one this change could get catastrophically
     * wrong. perform_leg_action is forbidden because an animation's contents
     * cannot be inspected first (D-010) -- WADDLE is what put him on the
     * floor. The halt entry pins the payload to exactly {STOP}; everything
     * else about that op must be refused exactly as before. */
    /* THE SENTINEL, like every other set_ceiling in this file. Without it
     * test_untouched_default silently stops testing anything: it asserts
     * the INITIALISER is READ only when nothing has set the ceiling, and
     * these tests set it. Measured -- with these four running first and
     * not tripping it, `s_ceiling = R2_TIER_STANCE` survived the whole
     * suite here while main killed it. That initialiser is the shipping
     * panel's ONLY ceiling: firmware/panel never calls set_ceiling. */
    s_ceiling_touched = 1; r2_gate_set_ceiling(R2_TIER_STANCE);

    const uint8_t waddle[]   = { 3 };             /* LEG_ACTION_WADDLE */
    const uint8_t three[]    = { 1 };
    const uint8_t two[]      = { 2 };
    const uint8_t stop_plus[] = { 0, 0 };         /* STOP, then something else */
    const uint8_t stop[]     = { 0 };             /* LEG_ACTION_STOP */

    CHECK(r2_gate_check(0x17, 0x0D, waddle, 1) == R2_GATE_FORBIDDEN,
          "WADDLE got through the halt list");
    CHECK(r2_gate_check(0x17, 0x0D, three, 1) == R2_GATE_FORBIDDEN,
          "THREE_LEGS got through the halt list");
    CHECK(r2_gate_check(0x17, 0x0D, two, 1) == R2_GATE_FORBIDDEN,
          "TWO_LEGS got through the halt list");
    /* A LONGER PAYLOAD STARTING WITH STOP IS A DIFFERENT COMMAND. A prefix
     * match here would admit an arbitrary tail. */
    CHECK(r2_gate_check(0x17, 0x0D, stop_plus, 2) == R2_GATE_FORBIDDEN,
          "a two-byte payload beginning with STOP was treated as a halt");
    /* And no payload at all is not the halt either -- the halt is one byte. */
    CHECK(r2_gate_check(0x17, 0x0D, NULL, 0) == R2_GATE_FORBIDDEN,
          "an empty perform_leg_action was treated as a halt");

    /* A LENGTH WITH NO BYTES IS NOT A HALT. A caller that passes NULL and
     * claims one byte is malformed, and the gate must refuse rather than
     * believe the length -- a mutation battery flipped this guard to `return
     * true` and nothing noticed, which is a halt entry that admits an op
     * whose payload was never actually read. */
    CHECK(r2_gate_check(0x17, 0x0D, NULL, 1) == R2_GATE_FORBIDDEN,
          "a NULL payload claiming one byte was treated as the legs STOP");

    CHECK(r2_gate_check(0x17, 0x0D, stop, 1) == R2_GATE_ALLOW,
          "the legs STOP was refused");

    s_ceiling_touched = 1; r2_gate_set_ceiling(R2_TIER_READ);
}

static void test_every_halt_is_admitted_at_every_ceiling(void)
{
    /* THE WHOLE POINT. A stop you have to raise a ceiling to reach is not a
     * stop, and the panel's ceiling is READ. */
    const uint8_t stop[] = { 0 };
    for (int c = R2_TIER_READ; c < R2_TIER__COUNT; c++) {
        s_ceiling_touched = 1; r2_gate_set_ceiling((r2_tier_t)c);
        CHECK(r2_gate_check(0x17, 0x2B, NULL, 0) == R2_GATE_ALLOW,
              "stop_animation refused at ceiling %d", c);
        CHECK(r2_gate_check(0x1A, 0x0A, NULL, 0) == R2_GATE_ALLOW,
              "stop_audio refused at ceiling %d", c);
        CHECK(r2_gate_check(0x17, 0x0D, stop, 1) == R2_GATE_ALLOW,
              "the legs STOP refused at ceiling %d", c);
    }
    s_ceiling_touched = 1; r2_gate_set_ceiling(R2_TIER_READ);
}

static void test_the_halt_list_widens_nothing_else(void)
{
    /* A list checked BEFORE forbidden is the most dangerous kind of list.
     * Sweep the whole did/cid space at the most permissive ceiling and assert
     * that the ONLY verdicts that changed are the three halts. Anything else
     * newly allowed would be this change leaking. */
    /* THE EXPECTED SET, written out longhand. Asserting "the op I thought of
     * is still refused" is the data-not-the-guard shape: a ROGUE FOURTH HALT
     * ENTRY for an op that is merely NOT_ALLOWLISTED survived the earlier
     * version of this test, because nothing compared the sweep against a
     * baseline. This is that baseline. */
    static const struct { uint8_t did, cid; } MAY_ALLOW[] = {
        { 0x13, 0x0D }, { 0x13, 0x03 },                 /* wake, battery */
        { 0x17, 0x14 }, { 0x17, 0x25 }, { 0x17, 0x16 }, /* head, leg reads */
        { 0x17, 0x2B },                                 /* stop_animation */
        { 0x18, 0x01 }, { 0x11, 0x00 },                 /* sensors, version */
        { 0x1A, 0x0E },                                 /* leds */
        { 0x1A, 0x07 }, { 0x1A, 0x08 }, { 0x1A, 0x0A }, /* audio */
        { 0x17, 0x0F },                                 /* dome */
    };
    s_ceiling_touched = 1; r2_gate_set_ceiling(R2_TIER_STANCE);
    for (unsigned did = 0; did < 256u; did++) {
        for (unsigned cid = 0; cid < 256u; cid++) {
            /* Empty payload: the legs halt needs one byte, so it is not in
             * play here and every other op must answer as it always did. */
            const r2_gate_verdict_t v =
                r2_gate_check((uint8_t)did, (uint8_t)cid, NULL, 0);
            if (v != R2_GATE_ALLOW) continue;
            int expected = 0;
            for (size_t k = 0; k < sizeof MAY_ALLOW / sizeof MAY_ALLOW[0]; k++)
                if (MAY_ALLOW[k].did == did && MAY_ALLOW[k].cid == cid)
                    expected = 1;
            CHECK(expected, "%02x/%02x is newly ALLOWED at the top ceiling",
                  did, cid);
        }
    }
    s_ceiling_touched = 1; r2_gate_set_ceiling(R2_TIER_READ);
}

static void test_a_halt_actually_reaches_the_transport(void)
{
    /* PROVE THE GATE AT THE EFFECTOR. A verdict function that says ALLOW
     * proves nothing about what r2_gate_send does with it -- this repo's own
     * rule. Drive the real send and read the bytes that reach tx. */
    s_ceiling_touched = 1; r2_gate_set_ceiling(R2_TIER_READ);
    const uint8_t stop[] = { 0 };
    tx_reset();
    const int n = r2_gate_send(0x17, 0x0D, 1, stop, 1, fake_tx, NULL);
    CHECK(n > 0, "the legs STOP did not send (%d)", n);
    CHECK(tx_calls == 1, "the legs STOP reached tx %d times", tx_calls);

    /* AND THE BYTES ON THE WIRE ARE THE ONES THAT WERE VALIDATED. Counting
     * tx calls proves the gate said yes; it does not prove WHAT went out.
     * A mutant that validated {STOP} and then encoded {WADDLE} before
     * transmitting survived a test that only counted calls -- a
     * time-of-check/time-of-use hole, unpinned. Compare against the frame the
     * packet layer makes for the payload we asked for. */
    uint8_t want[R2_ENCODED_MAX(8)];
    const int wn = r2_packet_encode(0x17, 0x0D, 1, stop, 1, want, sizeof want);
    CHECK(wn > 0, "could not encode the expected STOP frame");
    CHECK(tx_len == (size_t)wn && memcmp(tx_buf, want, (size_t)wn) == 0,
          "the bytes that reached the transport are not the STOP frame that "
          "was validated");

    /* And the motion still does not. */
    const uint8_t waddle[] = { 3 };
    tx_reset();
    const int m = r2_gate_send(0x17, 0x0D, 2, waddle, 1, fake_tx, NULL);
    CHECK(m == R2_GATE_FORBIDDEN, "WADDLE was not refused by send (%d)", m);
    CHECK(tx_calls == 0, "WADDLE reached the transport %d times", tx_calls);
}

/* ---- THE STATUS PATH (D-030) --------------------------------------------- */

/* Every op in the gate's ALLOWED and FORBIDDEN tables except the light
 * itself, plus ops it does not know. None may leave by the status path, grant
 * or no grant. */
static const struct { uint8_t did, cid; const char *name; } NOT_LIGHTS[] = {
    { 0x13, 0x0D, "wake" },           { 0x13, 0x03, "battery_voltage" },
    { 0x17, 0x14, "get_head_position" },
    { 0x17, 0x2B, "stop_animation" }, { 0x1A, 0x07, "play_audio" },
    { 0x1A, 0x08, "set_volume" },     { 0x1A, 0x0A, "stop_audio" },
    { 0x17, 0x0F, "set_head_position" },
    { 0x17, 0x0D, "perform_leg_action" }, { 0x16, 0x07, "drive" },
    { 0x13, 0x01, "sleep" },          { 0x1A, 0x1C, "set_leds_8bit" },
    { 0x17, 0x05, "play_animation" }, { 0x13, 0x00, "enter_deep_sleep" },
    { 0x17, 0x15, "set_leg_position" }, { 0x17, 0x25, "get_leg_action" },
    { 0x17, 0x16, "get_leg_position" }, { 0x18, 0x01, "get_sensor_mask" },
    { 0x11, 0x00, "get_main_app_version" },
    { 0x7F, 0x7F, "unknown" },
    /* same CID as the light, wrong device: the match must be on BOTH */
    { 0x13, 0x0E, "power 0x0E" },     { 0x17, 0x0E, "animatronic 0x0E" },
};
#define NOT_LIGHTS_N (sizeof NOT_LIGHTS / sizeof NOT_LIGHTS[0])

static void test_status_path_admits_nothing_but_lights(void)
{
    printf("     the status path refuses every op that is not a light, granted or not\n");
    /* A PERFECT light payload under the wrong op, so the shape check cannot
     * be what refuses it -- only the op check can. With a one-byte payload
     * the shape check shadowed it and deleting the op check stayed green. */
    const uint8_t one[] = {0x00, 0xFF, 1, 2, 3, 4, 5, 6, 7, 8};
    for (int g = 0; g < 2; g++) {
        r2_gate_grant_status(g == 1);
        for (size_t i = 0; i < NOT_LIGHTS_N; i++) {
            tx_reset();
            const int n = r2_gate_send_status(NOT_LIGHTS[i].did, NOT_LIGHTS[i].cid,
                                              1, one, sizeof one, fake_tx, NULL);
            CHECK(n == R2_GATE_NOT_STATUS && tx_calls == 0,
                  "%s left by the status path (grant %d, got %d, tx %d)",
                  NOT_LIGHTS[i].name, g, n, tx_calls);
        }
    }
    r2_gate_grant_status(false);
}

static void test_status_path_admits_only_the_full_frame(void)
{
    printf("     the status light must carry all eight channels, and nothing else\n");
    r2_gate_grant_status(true);
    const uint8_t partial[] = {0x00, 0x77, 0, 0, 255, 0, 0, 255};
    const uint8_t short_[]  = {0x00, 0xFF, 1, 2, 3};
    const uint8_t long_[]   = {0x00, 0xFF, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    const uint8_t highbit[] = {0x01, 0xFF, 1, 2, 3, 4, 5, 6, 7, 8};
    const struct { const uint8_t *d; size_t n; const char *what; } bad[] = {
        { partial, sizeof partial, "mask 0x0077" }, { short_, sizeof short_, "short" },
        { long_, sizeof long_, "long" },            { highbit, sizeof highbit, "mask 0x01FF" },
        { NULL, 10, "NULL payload" },
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        tx_reset();
        const int n = r2_gate_send_status(0x1A, 0x0E, 1, bad[i].d, bad[i].n, fake_tx, NULL);
        CHECK(n == R2_GATE_NOT_STATUS && tx_calls == 0,
              "%s left by the status path (got %d)", bad[i].what, n);
    }
    r2_gate_grant_status(false);
}

static void test_the_grant_widens_nothing_on_the_main_path(void)
{
    printf("     holding the grant changes no verdict on r2_gate_send\n");
    s_ceiling_touched = 1; r2_gate_set_ceiling(R2_TIER_READ);
    r2_gate_grant_status(true);
    CHECK(r2_gate_check(0x1A, 0x0E, NULL, 0) == R2_GATE_ABOVE_CEILING,
          "the grant must not open LEDs on the ceiling-governed path");
    const uint8_t leds[] = {0x00, 0x01, 0xFF};
    tx_reset();
    CHECK(r2_gate_send(0x1A, 0x0E, 1, leds, sizeof leds, fake_tx, NULL)
              == R2_GATE_ABOVE_CEILING && tx_calls == 0,
          "an LED write through r2_gate_send must still be refused at READ");
    CHECK(r2_gate_get_ceiling() == R2_TIER_READ, "the grant must not move the ceiling");
    r2_gate_grant_status(false);
}

static void test_granted_light_goes_out_and_revoke_stops_it(void)
{
    printf("     granted, a light goes out with the packet layer's bytes; revoked, it stops\n");
    const uint8_t blue[] = {0x00, 0xFF, 0, 0, 255, 0, 0, 0, 255, 0};
    uint32_t a0, r0, a1, r1;
    r2_gate_grant_status(true);
    r2_gate_stats(&a0, &r0);
    tx_reset();
    const int n = r2_gate_send_status(0x1A, 0x0E, 5, blue, sizeof blue, fake_tx, NULL);
    r2_gate_stats(&a1, &r1);
    uint8_t want[32];
    const int wn = r2_packet_encode(0x1A, 0x0E, 5, blue, sizeof blue, want, sizeof want);
    CHECK(wn > 0 && n == wn && tx_calls == 1 && tx_len == (size_t)wn &&
          memcmp(tx_buf, want, (size_t)wn) == 0,
          "a granted light must leave once, byte for byte (n %d, tx %d)", n, tx_calls);
    CHECK(a1 == a0 + 1 && r1 == r0, "the admitted counter must move by exactly one");

    r2_gate_grant_status(false);
    tx_reset();
    CHECK(r2_gate_send_status(0x1A, 0x0E, 6, blue, sizeof blue, fake_tx, NULL)
              == R2_GATE_NOT_GRANTED && tx_calls == 0,
          "after the grant is revoked, nothing may leave");
    CHECK(r2_gate_send_status(0x1A, 0x0E, 6, blue, sizeof blue, NULL, NULL)
              == R2_GATE_NOT_GRANTED,
          "revoked beats a missing tx: the refusal must name the grant");
    r2_gate_grant_status(true);
    CHECK(r2_gate_send_status(0x1A, 0x0E, 7, blue, sizeof blue, NULL, NULL)
              == R2_GATE_NO_TX, "granted with no tx must be refused, not crash");
    r2_gate_grant_status(false);
}

/* ---- THE REPLY PATH (D-032, 3.5a's audio half) --------------------------- */

/* A committed id, for tests that need one that is NOT the illegal case under
 * test. Must appear in r2_gate.c's REPLY_CHIRP_IDS -- 1966 is R2_CHATTY_11. */
#define CHIRP_ID_OK 1966u
/* A plausible-looking id that is NOT in the committed table. Sphero sound ids
 * run into the thousands, so a small number reads as "obviously synthetic"
 * without needing to invent a fake DID/CID pair. */
#define CHIRP_ID_UNKNOWN 1u

/* Ever-increasing, matching panel_exchange's own invariant (ids never repeat)
 * -- so each test can assume a fresh id has never spent its chirp, with no
 * reset function needed. */
static uint32_t next_test_exchange_id(void)
{
    static uint32_t id = 1000;
    return ++id;
}

static void test_reply_chirp_needs_the_wake_grant(void)
{
    printf("     a reply chirp is refused with no WAKE grant, whatever may_act says\n");
    r2_gate_grant_status(false);
    const uint32_t id = next_test_exchange_id();
    tx_reset();
    /* may_act TRUE on purpose: the grant must be checked independently of
     * what the caller claims about the exchange, the same way send_status's
     * op check does not care whether the grant is held. */
    const int n = r2_gate_send_reply_audio(true, id, CHIRP_ID_OK, 1, fake_tx, NULL);
    CHECK(n == R2_GATE_NOT_GRANTED && tx_calls == 0,
          "an ungranted reply chirp went out (got %d, tx %d)", n, tx_calls);
}

static void test_reply_chirp_needs_may_act(void)
{
    printf("     a reply chirp is refused when the caller says may_act is false\n");
    r2_gate_grant_status(true);
    const uint32_t id = next_test_exchange_id();
    tx_reset();
    const int n = r2_gate_send_reply_audio(false, id, CHIRP_ID_OK, 1, fake_tx, NULL);
    CHECK(n == R2_GATE_REPLY_NOT_LIVE && tx_calls == 0,
          "a reply chirp with may_act=false went out (got %d, tx %d)", n, tx_calls);
    r2_gate_grant_status(false);
}

static void test_reply_chirp_needs_a_committed_id(void)
{
    printf("     a reply chirp with an id outside the committed table is refused\n");
    r2_gate_grant_status(true);
    const uint32_t id1 = next_test_exchange_id();
    tx_reset();
    int n = r2_gate_send_reply_audio(true, id1, CHIRP_ID_UNKNOWN, 1, fake_tx, NULL);
    CHECK(n == R2_GATE_REPLY_BAD_ID && tx_calls == 0,
          "an uncommitted sound id went out (got %d, tx %d)", n, tx_calls);
    /* id 0 (VOICE_MOOD_NONE's sentinel, r2_ops.c) must refuse the same way. */
    const uint32_t id2 = next_test_exchange_id();
    tx_reset();
    n = r2_gate_send_reply_audio(true, id2, 0, 1, fake_tx, NULL);
    CHECK(n == R2_GATE_REPLY_BAD_ID && tx_calls == 0,
          "sound id 0 went out (got %d, tx %d)", n, tx_calls);
    r2_gate_grant_status(false);
}

static void test_reply_chirp_is_at_most_one_per_exchange(void)
{
    printf("     a second chirp for the same exchange is refused, a first for a new one is not\n");
    r2_gate_grant_status(true);
    const uint32_t id = next_test_exchange_id();
    tx_reset();
    int n = r2_gate_send_reply_audio(true, id, CHIRP_ID_OK, 1, fake_tx, NULL);
    CHECK(n > 0 && tx_calls == 1, "the first chirp for a fresh exchange was refused (%d)", n);

    tx_reset();
    n = r2_gate_send_reply_audio(true, id, CHIRP_ID_OK, 2, fake_tx, NULL);
    CHECK(n == R2_GATE_REPLY_USED && tx_calls == 0,
          "a second chirp for the SAME exchange went out (got %d, tx %d)", n, tx_calls);

    /* A different (later) id is a different exchange and gets its own chirp. */
    const uint32_t id2 = next_test_exchange_id();
    tx_reset();
    n = r2_gate_send_reply_audio(true, id2, CHIRP_ID_OK, 3, fake_tx, NULL);
    CHECK(n > 0 && tx_calls == 1, "a fresh exchange inherited the prior one's used-up budget (%d)", n);
    r2_gate_grant_status(false);
}

static void test_reply_chirp_a_refused_call_never_spends_the_budget(void)
{
    /* A NO_TX call must not burn the exchange's one chirp -- otherwise a
     * caller that discovers it has no transport (or retries after a
     * transient refusal) permanently loses the reply for that exchange. */
    printf("     a call that never reaches the transport does not spend the exchange's chirp\n");
    r2_gate_grant_status(true);
    const uint32_t id = next_test_exchange_id();
    tx_reset();
    int n = r2_gate_send_reply_audio(true, id, CHIRP_ID_OK, 1, NULL, NULL);
    CHECK(n == R2_GATE_NO_TX, "a no-tx reply chirp returned %d, not NO_TX", n);

    n = r2_gate_send_reply_audio(true, id, CHIRP_ID_OK, 2, fake_tx, NULL);
    CHECK(n > 0 && tx_calls == 1,
          "the same exchange's chirp was refused after an earlier NO_TX call (%d)", n);
    r2_gate_grant_status(false);
}

/* A transport that reports FAILURE after being called -- found by adversarial
 * review. transmit()'s own rule elsewhere in this file is "assume an
 * unconfirmed command took effect": a tx() returning < 0 may still have put
 * bytes on the air. The FIRST version of this door only spent the exchange's
 * budget on a clean success, so a caller retrying after this exact ambiguous
 * failure could land a REAL second chirp. */
static int failing_tx(const uint8_t *f, size_t n, void *ctx)
{
    (void)f; (void)ctx;
    tx_calls++;
    tx_len = n;
    return -1;
}

static void test_reply_chirp_a_failed_transmit_still_spends_the_budget(void)
{
    printf("     a chirp whose transport reports failure still spends the exchange's budget\n");
    r2_gate_grant_status(true);
    const uint32_t id = next_test_exchange_id();
    tx_reset();
    int n = r2_gate_send_reply_audio(true, id, CHIRP_ID_OK, 1, failing_tx, NULL);
    CHECK(n < 0 && tx_calls == 1,
          "the failing transmit was not even attempted (n=%d, tx_calls=%d)", n, tx_calls);

    /* The retry a caller would naturally make after seeing a failure. It must
     * be refused as USED, not allowed to try again -- a real chirp may
     * already be on the air from the call above. */
    tx_reset();
    n = r2_gate_send_reply_audio(true, id, CHIRP_ID_OK, 2, fake_tx, NULL);
    CHECK(n == R2_GATE_REPLY_USED && tx_calls == 0,
          "a retry after an ambiguous tx failure sent a SECOND real chirp (got %d, tx %d)",
          n, tx_calls);
    r2_gate_grant_status(false);
}

static void test_reply_chirp_bytes_and_op(void)
{
    printf("     an admitted reply chirp is play_audio, with the id big-endian and PLAY_IMMEDIATELY\n");
    r2_gate_grant_status(true);
    const uint32_t id = next_test_exchange_id();
    tx_reset();
    const int n = r2_gate_send_reply_audio(true, id, CHIRP_ID_OK, 9, fake_tx, NULL);
    CHECK(n > 0, "the chirp was refused (%d)", n);
    const uint8_t payload[3] = { 0x07, 0xAE, 0x00 };  /* 1966 = 0x07AE, mode 0 */
    uint8_t want[R2_ENCODED_MAX(8)];
    const int wn = r2_packet_encode(0x1A, 0x07, 9, payload, sizeof payload, want, sizeof want);
    CHECK(wn > 0 && tx_len == (size_t)wn && memcmp(tx_buf, want, (size_t)wn) == 0,
          "reply chirp bytes differ from the hand-computed play_audio frame");
    r2_gate_grant_status(false);
}

/* THE FULL COMMITTED SET, written out longhand -- the same shape as
 * test_the_halt_list_widens_nothing_else's MAY_ALLOW baseline. A test that
 * only ever tries CHIRP_ID_OK cannot tell "the table is right" from "the
 * table admits at least one id"; an off-by-one that drops the LAST entry
 * (HEY_1) survived exactly that gap until this test existed. */
static void test_every_committed_chirp_id_is_admitted(void)
{
    printf("     every id in the 3.4a-committed table is admitted, not just one\n");
    static const uint16_t committed[] = {
        1966, 2007, 3302, 1910, 3101, 3484, 3703, 1737, 2813,
    };
    r2_gate_grant_status(true);
    for (size_t i = 0; i < sizeof committed / sizeof committed[0]; i++) {
        const uint32_t id = next_test_exchange_id();
        tx_reset();
        const int n = r2_gate_send_reply_audio(true, id, committed[i], 1, fake_tx, NULL);
        CHECK(n > 0 && tx_calls == 1,
              "committed id %u was refused (got %d)", committed[i], n);
    }
    r2_gate_grant_status(false);
}

static void test_reply_chirp_does_not_widen_the_main_gate(void)
{
    printf("     the reply path opens no door on r2_gate_check/r2_gate_send\n");
    r2_gate_grant_status(true);
    s_ceiling_touched = 1; r2_gate_set_ceiling(R2_TIER_READ);
    CHECK(r2_gate_check(0x1A, 0x07, NULL, 0) == R2_GATE_ABOVE_CEILING,
          "granting the reply path opened play_audio on the ceiling-governed path");
    tx_reset();
    CHECK(r2_gate_send(0x1A, 0x07, 1, NULL, 0, fake_tx, NULL) == R2_GATE_ABOVE_CEILING
              && tx_calls == 0,
          "play_audio left through r2_gate_send while only the reply grant was held");
    r2_gate_grant_status(false);
    s_ceiling_touched = 1; r2_gate_set_ceiling(R2_TIER_READ);
}

int main(void)
{
    printf("r2_gate host tests\n==================\n");
    test_untouched_default();          /* FIRST: the default is only observable now */
    test_forbidden_at_every_ceiling();
    test_unknown_ops_refused();
    test_above_ceiling_refused();
    test_clamping();
    test_allowed_ops_transmit();
    test_no_tx_is_refused_not_crashed();

    /* LAST, and test_untouched_default stays FIRST. Every one of these sets
     * the ceiling, and that test can only prove the INITIALISER is READ
     * while nothing has. Run ahead of it they made it vacuous: measured,
     * `s_ceiling = R2_TIER_STANCE` survived the whole suite here and was
     * killed on main. The panel never calls set_ceiling, so that
     * initialiser is the shipping ceiling. */
    test_a_leg_action_that_is_not_stop_is_still_forbidden();
    test_every_halt_is_admitted_at_every_ceiling();
    test_the_halt_list_widens_nothing_else();
    test_a_halt_actually_reaches_the_transport();
    test_status_path_admits_nothing_but_lights();
    test_status_path_admits_only_the_full_frame();
    test_the_grant_widens_nothing_on_the_main_path();
    test_granted_light_goes_out_and_revoke_stops_it();

    test_reply_chirp_needs_the_wake_grant();
    test_reply_chirp_needs_may_act();
    test_reply_chirp_needs_a_committed_id();
    test_reply_chirp_is_at_most_one_per_exchange();
    test_reply_chirp_a_refused_call_never_spends_the_budget();
    test_reply_chirp_a_failed_transmit_still_spends_the_budget();
    test_every_committed_chirp_id_is_admitted();
    test_reply_chirp_bytes_and_op();
    test_reply_chirp_does_not_widen_the_main_gate();
    printf("==================\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
