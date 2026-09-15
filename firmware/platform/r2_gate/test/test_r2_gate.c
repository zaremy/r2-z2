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
    printf("==================\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
