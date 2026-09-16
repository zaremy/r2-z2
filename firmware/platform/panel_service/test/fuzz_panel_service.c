/* THE OP CATALOGUE'S PUBLIC SURFACE, UNDER HOSTILE INPUT.
 *
 * `panel_service_ops_rows` is public and its documented contract is that it
 * takes ARBITRARY ops from arbitrary callers -- that is the whole reason it
 * exists as a separate function, because the shipped table gives only READ any
 * rows and every invariant asserted about actuator tiers would otherwise be
 * true, vacuous, and equally true with the rules deleted.
 *
 * A contract like that needs adversarial input, not three fixtures. Two real
 * defects came out of it that the example tests did not have: an aliased
 * `ops == out` call silently produced four identical bundle rows, and a count
 * larger than the caller's array was refused only AFTER the validation loop
 * had read past the end of it -- a heap-buffer-overflow READ, which is why
 * this is built with AddressSanitizer rather than run bare.
 *
 * AND THE FIRST VERSION COMMITTED HERE COULD NOT HAVE FOUND EITHER, which is
 * worth writing down because the file said otherwise for a commit. A review
 * instrumented it: 98% of cases were refused, and in 50,000 runs it produced
 * ZERO bundle rows -- so five of the seven invariants this banner lists could
 * not fire, and deleting the aliasing guard did not fail it, because the
 * generator never aliased anything. A fuzzer that cannot reach the code it
 * claims to guard is a green line meaning less than it looks like, which is
 * the same failure as a test asserting the shipped data is valid.
 *
 * Two things fix it, and both are about REACH rather than volume: the
 * generator now biases toward VALID rows, so multi-op lists survive validation
 * often enough to assemble bundles, and a share of cases are deliberately
 * aliased. Coverage is asserted at the end of the run rather than hoped for --
 * a run that stops producing bundle rows FAILS, instead of passing quietly the
 * way this one did.
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
        /* MOSTLY VALID, OCCASIONALLY NOT. A uniformly random row is invalid
         * about 85% of the time, so a uniformly random LIST is refused almost
         * always and the assembled-list invariants never get exercised -- the
         * first version of this file produced no bundle row at all in 50,000
         * cases. Biasing toward valid rows is what lets the interesting half
         * of the contract be reached; the invalid tail is still sampled, and
         * every rule still has its own example test besides. */
        const bool nasty = (rnd() % 16u) == 0u;

        ops[i].op    = nasty ? (panel_op_t)rnd_range(0, PANEL_OP__COUNT + 1)
                             : (panel_op_t)rnd_range(PANEL_OP_ALL + 1,
                                                     PANEL_OP__COUNT - 1);
        /* MOVING ONE ROW IN FOUR, not one in two. A bundle needs EVERY row
         * harmless, so at even odds a three-op list bundles one time in eight
         * and the bundle invariants stay nearly unreachable -- the coverage
         * assert at the end of main caught exactly that and failed the run,
         * which is the assert working. */
        ops[i].moves = (rnd() % 4u) == 0u;
        ops[i].sends = nasty ? (uint8_t)rnd_range(0, 4) : 1u;

        const int len = nasty ? rnd_range(0, PANEL_OP_NAME_LEN)
                              : rnd_range(1, PANEL_OP_NAME_LEN - 1);
        for (int k = 0; k < len; k++)
            ops[i].name[k] = (char)rnd_range('A', 'Z');
        if (len < PANEL_OP_NAME_LEN) ops[i].name[len] = '\0';
        /* len == PANEL_OP_NAME_LEN leaves it unterminated on purpose. The
         * `k < PANEL_OP_NAME_LEN` this loop used to also test was dead, `len`
         * being bounded by construction -- removed, in the spirit of the
         * conjunct the product code dropped for the same reason. */
    }
    return ops;
}

/* WHAT THE RUN ACTUALLY REACHED. Printed and ASSERTED at the end: a run that
 * stops assembling bundle rows, or stops accepting full-length lists, has lost
 * the coverage this file exists for and must fail rather than pass quietly.
 * That is exactly how the first version shipped looking green.
 *
 * THE THRESHOLDS ARE SET BY REVERTING EACH BIAS AND WATCHING THE RUN GO RED,
 * not by picking a number that sits under the current figures. Measured, one
 * bias reverted at a time: uniform rows -> 26 bundle rows, uniform tier -> 447,
 * moving one row in two -> 689, no aliasing -> 1 aliased call. The bundle
 * threshold is above 689 for that reason -- set below it, the generator could
 * lose a bias and stay green, which is the failure this whole mechanism exists
 * to prevent, and which it did once already. */
static unsigned s_accepted, s_bundles, s_full, s_aliased;

static void one_case(unsigned n)
{
    /* A BUNDLING TIER OFTEN, not one time in twelve. READ is the only tier
     * that may bundle, so a uniform tier draw puts the assembled-bundle
     * invariants out of reach however many cases are run. The out-of-range and
     * actuator tiers are still sampled heavily -- they are most of the draw. */
    const int tier  = ((rnd() % 3u) == 0u) ? 0 : rnd_range(-3, 8);
    const int n_ops = rnd_range(-2, PANEL_TIER_OPS_MAX + 2);
    panel_tier_op_t *ops = make_ops(n_ops);

    /* ALIASED, SOMETIMES. The guard against ops == out shipped with nothing
     * that could exercise it: removing the guard did not fail this fuzzer,
     * because the generator never passed the same pointer twice. */
    if (ops != NULL && (rnd() % 8u) == 0u) {
        s_aliased++;
        const int got_alias = panel_service_ops_rows(tier, ops, n_ops, ops);
        CHECK(got_alias == 0, "case %u: an aliased call returned %d rows",
              n, got_alias);
    }

    panel_tier_op_t out[PANEL_TIER_OPS_MAX];
    memset(out, 0xA5, sizeof out);
    const int got = panel_service_ops_rows(tier, ops, n_ops, out);

    CHECK(got >= 0 && got <= PANEL_TIER_OPS_MAX,
          "case %u: tier %d, %d ops -> %d rows", n, tier, n_ops, got);

    if (got > 0) {
        s_accepted++;
        if (n_ops == PANEL_TIER_OPS_MAX) s_full++;
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
        s_bundles += (unsigned)bundles;

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
         * over the subsets missing EXACTLY ONE single row -- not every proper
         * subset, as this comment used to claim. Those are the tightest cases
         * and a rule that accepts one of them accepts all the smaller ones,
         * but the sentence was broader than the loop. */
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

    /* COVERAGE IS A RESULT, NOT A HOPE. Asserted rather than printed, because
     * the version of this file that reached none of it still printed PASS. The
     * thresholds sit below what this generator reaches and above what each
     * single-bias regression produces. At 50k it accepts ~18,900 cases (37.7%
     * of cases) and assembles ~1,880 bundle rows (3.8% of cases; 10.0% of the
     * accepted ones) -- both quoted per-case, because an earlier version of
     * this sentence mixed the two denominators inside one parenthesis. */
    printf("  reach: accepted %u, bundle rows %u, full lists %u, aliased %u\n",
           s_accepted, s_bundles, s_full, s_aliased);
    CHECK(s_accepted > n / 20u, "only %u of %u cases were accepted", s_accepted, n);
    CHECK(s_bundles  > n / 50u, "only %u bundle rows in %u cases", s_bundles, n);
    CHECK(s_full     > n / 200u, "only %u full-length lists in %u cases", s_full, n);
    CHECK(s_aliased  > n / 100u, "only %u aliased calls in %u cases", s_aliased, n);

    printf("%s: %d checks, %d failures  (%u cases)\n",
           failures ? "FAIL" : "PASS", checks, failures, n);
    return failures ? 1 : 0;
}
