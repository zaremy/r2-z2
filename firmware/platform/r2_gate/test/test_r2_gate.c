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
            CHECK(r2_gate_check(FORBIDDEN[i].did, FORBIDDEN[i].cid) == R2_GATE_FORBIDDEN,
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
            r2_gate_verdict_t v = r2_gate_check((uint8_t)did, (uint8_t)cid);
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
        { 0x1A, 0x0A, "stop_audio" },
        { 0x17, 0x0F, "set_head_position" },
    };
    for (size_t i = 0; i < sizeof higher / sizeof higher[0]; i++) {
        CHECK(r2_gate_check(higher[i].did, higher[i].cid) == R2_GATE_ABOVE_CEILING,
              "%s must be above a read ceiling", higher[i].name);
        tx_reset();
        int rc = r2_gate_send(higher[i].did, higher[i].cid, 0, NULL, 0, fake_tx, NULL);
        CHECK(rc == R2_GATE_ABOVE_CEILING, "%s send must be refused", higher[i].name);
        CHECK(tx_calls == 0, "%s transmitted despite being above the ceiling",
              higher[i].name);
    }
    /* LEDs allowed at leds, audio still refused: the ladder is a ladder. */
    s_ceiling_touched = 1; r2_gate_set_ceiling(R2_TIER_LEDS);
    CHECK(r2_gate_check(0x1A, 0x0E) == R2_GATE_ALLOW, "leds allowed at the leds rung");
    CHECK(r2_gate_check(0x1A, 0x07) == R2_GATE_ABOVE_CEILING, "audio still refused at leds");
    CHECK(r2_gate_check(0x17, 0x0F) == R2_GATE_ABOVE_CEILING, "dome still refused at leds");
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
    CHECK(r2_gate_check(0x1A, 0x0E) == R2_GATE_ABOVE_CEILING,
          "with no ceiling set, an LED write must already be refused");
    CHECK(r2_gate_check(0x17, 0x0F) == R2_GATE_ABOVE_CEILING,
          "with no ceiling set, a dome move must already be refused");
}

static void test_clamping(void)
{
    printf("     an out-of-range ceiling clamps DOWN, never up\n");
    s_ceiling_touched = 1; r2_gate_set_ceiling((r2_tier_t)99);
    CHECK(r2_gate_get_ceiling() == R2_TIER_READ,
          "an out-of-range ceiling must clamp DOWN, never up");
    CHECK(r2_gate_check(0x17, 0x0F) == R2_GATE_ABOVE_CEILING,
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
    printf("==================\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
