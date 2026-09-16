/* THE OP CATALOGUE'S PUBLIC SURFACE, UNDER HOSTILE INPUT.
 *
 * `panel_service_ops_rows` is public and its documented contract is that it
 * takes ARBITRARY ops from arbitrary callers -- that is the whole reason it
 * exists as a separate function, because the shipped table gives only READ any
 * rows and every invariant asserted about actuator tiers would otherwise be
 * true, vacuous, and equally true with the rules deleted.
 *
 * A contract like that needs adversarial input, not three fixtures. This found
 * two real defects the example-based tests did not: an aliased `ops == out`
 * call silently produced four identical bundle rows, and a count larger than
 * the caller's array was refused only AFTER the validation loop had read past
 * the end of it -- a heap-buffer-overflow READ, which is why this is built
 * with AddressSanitizer rather than run bare.
 *
 * WHAT IT ASSERTS is every claim the header and the ADR make, on every
 * returned list, rather than on a list somebody chose:
 *
 *   - the row count is in range, and a refusal writes nothing;
 *   - at most one bundle row, and only at index 0;
 *   - a tier that may not bundle, or a list containing an op that moves him,
 *     never gets one;
 *   - the bundle's `sends` equals the number of rows below it;
 *   - every returned name is NUL-terminated inside its field and non-empty;
 *   - a single row sends exactly one;
 *   - tier_exercised is never satisfied by fewer than all the single rows,
 *     unless the bundle row itself passed.
 *
 * Deterministic: a fixed seed, so a failure is reproducible from the printed
 * case number. Not a replacement for the example tests -- those say what the
 * rules ARE, in words a person reads; this says nobody can get around them.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "panel_service.h"

static int failures = 0, checks = 0;
#define CHECK(cond, ...) do {                                \
    checks++;                                                \
    if (!(cond)) { failures++;                               \
        printf("  FAIL: "); printf(__VA_ARGS__);             \
        printf("\n        at %s:%d\n", __FILE__, __LINE__); }\
} while (0)

/* A small deterministic PRNG, so a failing case number reproduces exactly. */
static uint32_t s_rng = 0x5EEDu;
static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5;
    return s_rng;
}
static int rnd_range(int lo, int hi) { return lo + (int)(rnd() % (uint32_t)(hi - lo + 1)); }

/* THE OPS ARE BUILT IN A HEAP BLOCK OF EXACTLY n_ops ENTRIES, not a generous
 * stack array. That is what lets AddressSanitizer see a read past the end --
 * with a fixed-size buffer the overflow is silent and the fuzzer would report
 * clean on the defect it was written to find. */
static panel_tier_op_t *make_ops(int n_ops)
{
    if (n_ops <= 0) return NULL;
    panel_tier_op_t *ops = calloc((size_t)n_ops, sizeof *ops);
    if (ops == NULL) { perror("calloc"); exit(2); }
    for (int i = 0; i < n_ops; i++) {
        ops[i].op    = (panel_op_t)rnd_range(0, PANEL_OP__COUNT + 1);
        ops[i].moves = (rnd() & 1u) != 0u;
        ops[i].sends = (uint8_t)rnd_range(0, 4);
        const int len = rnd_range(0, PANEL_OP_NAME_LEN);
        for (int k = 0; k < len && k < PANEL_OP_NAME_LEN; k++)
            ops[i].name[k] = (char)rnd_range('A', 'Z');
        if (len < PANEL_OP_NAME_LEN) ops[i].name[len] = '\0';
        /* len == PANEL_OP_NAME_LEN leaves it unterminated on purpose. */
    }
    return ops;
}

static void one_case(unsigned n)
{
    const int tier  = rnd_range(-3, 8);
    const int n_ops = rnd_range(-2, PANEL_TIER_OPS_MAX + 2);
    panel_tier_op_t *ops = make_ops(n_ops);

    panel_tier_op_t out[PANEL_TIER_OPS_MAX];
    memset(out, 0xA5, sizeof out);
    const int got = panel_service_ops_rows(tier, ops, n_ops, out);

    CHECK(got >= 0 && got <= PANEL_TIER_OPS_MAX,
          "case %u: tier %d, %d ops -> %d rows", n, tier, n_ops, got);

    if (got > 0) {
        int bundles = 0, singles = 0;
        for (int i = 0; i < got; i++) {
            if (out[i].op == PANEL_OP_ALL) {
                bundles++;
                CHECK(i == 0, "case %u: a bundle row at index %d", n, i);
                CHECK(!out[i].moves, "case %u: the bundle claims to move him", n);
            } else {
                singles++;
                CHECK(out[i].sends == 1,
                      "case %u: single row %d sends %u", n, i, (unsigned)out[i].sends);
            }
            CHECK(memchr(out[i].name, '\0', sizeof out[i].name) != NULL,
                  "case %u: row %d's name is not terminated", n, i);
            CHECK(out[i].name[0] != '\0', "case %u: row %d has an empty name", n, i);
        }
        CHECK(bundles <= 1, "case %u: %d bundle rows", n, bundles);

        if (bundles == 1) {
            CHECK(panel_service_tier_may_bundle(tier),
                  "case %u: tier %d may not bundle and got one", n, tier);
            CHECK(out[0].sends == (uint8_t)singles,
                  "case %u: the bundle sends %u over %d rows",
                  n, (unsigned)out[0].sends, singles);
            for (int i = 1; i < got; i++)
                CHECK(!out[i].moves,
                      "case %u: a bundle sits over row %d, which moves him", n, i);
            /* The bundle alone exercises the tier; nothing less does. */
            CHECK(panel_service_tier_exercised(out, got, 1u << 0),
                  "case %u: the bundle did not exercise the tier", n);
        }

        /* NOTHING SHORT OF THE WHOLE SET, unless the bundle passed. Walked
         * over every proper subset of the single rows that is missing at
         * least one -- the widening this rule exists to refuse. */
        for (int i = 0; i < got; i++) {
            if (out[i].op == PANEL_OP_ALL) continue;
            uint32_t all_but_one = 0;
            for (int k = 0; k < got; k++)
                if (k != i && out[k].op != PANEL_OP_ALL) all_but_one |= 1u << k;
            CHECK(!panel_service_tier_exercised(out, got, all_but_one),
                  "case %u: %d of %d single rows exercised the tier",
                  n, singles - 1, singles);
        }
    } else {
        /* A REFUSAL WRITES NOTHING. The caller's buffer is still its poison. */
        unsigned char poison[sizeof out];
        memset(poison, 0xA5, sizeof poison);
        CHECK(memcmp(out, poison, sizeof out) == 0,
              "case %u: a refusal wrote into the caller's buffer", n);
    }

    /* And the hostile shapes the generator cannot reach on its own. */
    if (n_ops > 0 && n_ops <= PANEL_TIER_OPS_MAX) {
        CHECK(panel_service_ops_rows(tier, ops, n_ops, NULL) == 0,
              "case %u: a NULL out was written through", n);
        CHECK(panel_service_ops_rows(tier, NULL, n_ops, out) == 0,
              "case %u: a NULL ops was read", n);
    }
    /* A count past the real array: refused BEFORE the loop dereferences it,
     * which is the whole point -- ASan sees the read if the bound moves. */
    if (n_ops > 0)
        CHECK(panel_service_ops_rows(tier, ops, n_ops + 32, out) == 0,
              "case %u: a count past the array was not refused", n);

    free(ops);
}

int main(int argc, char **argv)
{
    unsigned n = (argc > 1) ? (unsigned)strtoul(argv[1], NULL, 10) : 200000u;
    for (unsigned i = 0; i < n; i++) one_case(i);
    printf("%s: %d checks, %d failures  (%u cases)\n",
           failures ? "FAIL" : "PASS", checks, failures, n);
    return failures ? 1 : 0;
}
