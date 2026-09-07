/* Host tests for the panel's state universe (#101 AC2 / AC2b / AC2c).
 *
 * THE PROPERTY UNDER TEST IS THE GUARD, NOT THE TABLE. Asserting that the
 * shipped rows are well-formed is the test this repo has been burned by twice:
 * it passes with the check deleted, because every row somebody wrote by hand
 * happens to be legal. So the illegal case comes first in each group -- an
 * unranked state offered a conflict it must not win, a state past the end of
 * the enum, an empty mask -- and the legal walk comes after.
 */
#include <stdio.h>
#include <stddef.h>
#include <string.h>

#include "panel_state.h"

static int failures = 0, checks = 0;

#define CHECK(cond, ...) do {                                \
    checks++;                                                \
    if (!(cond)) { failures++;                               \
        printf("  FAIL: "); printf(__VA_ARGS__);             \
        printf("\n        at %s:%d\n", __FILE__, __LINE__); }\
} while (0)

#define BIT(s) (1u << (unsigned)(s))

/* ---- the rank is a closed set, and -1 is not a severity ----------------- */

static void test_unranked_states_report_minus_one(void)
{
    /* NOT "unranked states rank last". A large sentinel would let an unranked
     * state lose a comparison quietly and correctly today, then win one the
     * moment somebody compares with > instead of <. -1 forces the caller to
     * ask the question. */
    const panel_state_t outside[] = {
        PANEL_ST_RELEASED, PANEL_ST_WAKING, PANEL_ST_UNPROVISIONED,
    };
    for (unsigned i = 0; i < sizeof outside / sizeof outside[0]; i++) {
        CHECK(panel_state_rank(outside[i]) == -1,
              "%s ranked %d, expected -1", panel_state_name(outside[i]),
              panel_state_rank(outside[i]));
        CHECK(!panel_state_is_ranked(outside[i]),
              "%s reported as ranked", panel_state_name(outside[i]));
    }
}

static void test_out_of_range_is_refused(void)
{
    CHECK(panel_state_rank((panel_state_t)PANEL_ST_COUNT) == -1,
          "a state past the end of the enum was given a rank");
    CHECK(panel_state_rank((panel_state_t)-1) == -1,
          "a negative state was given a rank");
    CHECK(panel_state_colour((panel_state_t)PANEL_ST_COUNT) == 0u,
          "an out-of-range state produced a plausible colour");
    CHECK(panel_state_word((panel_state_t)PANEL_ST_COUNT)[0] == '\0',
          "an out-of-range state produced a word");
    CHECK(panel_state_since((panel_state_t)PANEL_ST_COUNT, PANEL_OFF_R2)[0] == '\0',
          "an out-of-range state produced a reason");
}

static void test_the_nine_are_ranked_in_enum_order(void)
{
    CHECK((int)PANEL_ST_RANKED_COUNT == 9,
          "the rank is a CLOSED nine-state set (D-017); found %d",
          (int)PANEL_ST_RANKED_COUNT);
    for (int i = 0; i < (int)PANEL_ST_RANKED_COUNT; i++)
        CHECK(panel_state_rank((panel_state_t)i) == i,
              "%s ranked %d, expected %d",
              panel_state_name((panel_state_t)i),
              panel_state_rank((panel_state_t)i), i);
}

/* ---- resolution: an unranked state may not win ------------------------- */

static void test_unranked_cannot_beat_ranked(void)
{
    /* The failure this exists to catch: `waking` is set because the link is
     * coming up, and `danger` is set because he has fallen over. If waking
     * could win, the panel would announce a reconnect while he is on the
     * floor. Every unranked state is tried against the WEAKEST ranked one, so
     * the test fails if any of them wins anything at all. */
    const panel_state_t outside[] = {
        PANEL_ST_RELEASED, PANEL_ST_WAKING, PANEL_ST_UNPROVISIONED,
    };
    for (unsigned i = 0; i < sizeof outside / sizeof outside[0]; i++) {
        const uint32_t mask = BIT(outside[i]) | BIT(PANEL_ST_SLEEP);
        CHECK(panel_state_resolve(mask) == PANEL_ST_SLEEP,
              "%s beat the least severe ranked state",
              panel_state_name(outside[i]));
    }
    /* And alone, an unranked state resolves to nothing rather than to itself:
     * the caller displays it deliberately, it is never the winner of a
     * contest. */
    CHECK(panel_state_resolve(BIT(PANEL_ST_WAKING)) == PANEL_ST_COUNT,
          "an unranked state won an uncontested resolution");
}

static void test_empty_mask_has_no_default(void)
{
    /* PANEL_ST_COUNT, not PANEL_ST_IDLE. A default of `idle` is how a panel
     * ends up asserting that nothing is engaged about a droid it has heard
     * nothing from -- the same class as a needle parked at zero. */
    CHECK(panel_state_resolve(0u) == PANEL_ST_COUNT,
          "an empty mask produced a state");
}

static void test_severity_order_holds_pairwise(void)
{
    /* Every ranked pair, not a sample: the rank's whole job is the ordering,
     * and a spot-check of the extremes passes with the middle shuffled. */
    for (int a = 0; a < (int)PANEL_ST_RANKED_COUNT; a++) {
        for (int b = 0; b < (int)PANEL_ST_RANKED_COUNT; b++) {
            const panel_state_t expect = (a < b) ? (panel_state_t)a : (panel_state_t)b;
            CHECK(panel_state_resolve(BIT(a) | BIT(b)) == expect,
                  "%s vs %s resolved to %s",
                  panel_state_name((panel_state_t)a),
                  panel_state_name((panel_state_t)b),
                  panel_state_name(panel_state_resolve(BIT(a) | BIT(b))));
        }
    }
    /* All nine at once must give danger, which is the case that matters. */
    uint32_t all = 0;
    for (int i = 0; i < (int)PANEL_ST_RANKED_COUNT; i++) all |= BIT(i);
    CHECK(panel_state_resolve(all) == PANEL_ST_DANGER,
          "everything wrong at once did not resolve to danger");
}

/* ---- the wake frame ---------------------------------------------------- */

static void test_wake_frame_fires_for_exactly_three(void)
{
    /* Written as a full sweep with an explicit expectation per state, so
     * adding a state without deciding whether it wakes fails here rather than
     * silently defaulting to quiet. AC3: attention, offline, danger. */
    for (int i = 0; i < (int)PANEL_ST_COUNT; i++) {
        const panel_state_t s = (panel_state_t)i;
        const bool expect = (s == PANEL_ST_DANGER ||
                             s == PANEL_ST_OFFLINE ||
                             s == PANEL_ST_ATTENTION);
        CHECK(panel_state_wakes(s) == expect,
              "%s wakes=%d, expected %d", panel_state_name(s),
              (int)panel_state_wakes(s), (int)expect);
    }
}

static void test_no_unranked_state_wakes(void)
{
    /* D-023 states this as a rule in its own right, so it gets its own check
     * rather than riding on the sweep above: the wake frame surfaces SEVERITY,
     * and none of the three is one. Without this, ranking one of them later
     * would quietly also make it wake the household. */
    for (int i = (int)PANEL_ST_RANKED_COUNT; i < (int)PANEL_ST_COUNT; i++)
        CHECK(!panel_state_wakes((panel_state_t)i),
              "%s, which is outside the rank, fires the wake frame",
              panel_state_name((panel_state_t)i));
}

/* ---- offline is ONE state with three views ----------------------------- */

static void test_offline_modes_give_distinct_reasons(void)
{
    const char *r2  = panel_state_since(PANEL_ST_OFFLINE, PANEL_OFF_R2);
    const char *net = panel_state_since(PANEL_ST_OFFLINE, PANEL_OFF_NET);
    const char *llm = panel_state_since(PANEL_ST_OFFLINE, PANEL_OFF_LLM);
    CHECK(strcmp(r2, net) != 0 && strcmp(net, llm) != 0 && strcmp(r2, llm) != 0,
          "the three offline views do not say which thing is unreachable");
    /* One state, one word: the distinction is in the reason line, not the
     * headline. Three headlines would be three states by another name. */
    CHECK(strcmp(panel_state_word(PANEL_ST_OFFLINE), "OFFLINE") == 0,
          "offline's word changed with the view");
    CHECK(panel_state_since(PANEL_ST_OFFLINE, (panel_offline_mode_t)PANEL_OFF_COUNT)[0] == '\0',
          "an out-of-range offline mode produced a reason");
}

static void test_mode_is_ignored_by_every_other_state(void)
{
    for (int i = 0; i < (int)PANEL_ST_COUNT; i++) {
        if (i == (int)PANEL_ST_OFFLINE) continue;
        const char *a = panel_state_since((panel_state_t)i, PANEL_OFF_R2);
        const char *b = panel_state_since((panel_state_t)i, PANEL_OFF_LLM);
        CHECK(strcmp(a, b) == 0,
              "%s changed its reason with the offline view",
              panel_state_name((panel_state_t)i));
    }
}

/* ---- the table is complete -------------------------------------------- */

static void test_every_state_renders(void)
{
    /* Not "the rows are pretty" -- that a state added to the enum without a
     * row is caught here rather than as a blank word on the glass. */
    for (int i = 0; i < (int)PANEL_ST_COUNT; i++) {
        const panel_state_t s = (panel_state_t)i;
        CHECK(panel_state_name(s)[0] != '\0', "state %d has no name", i);
        CHECK(panel_state_word(s)[0] != '\0',
              "%s has no word for the glass", panel_state_name(s));
        CHECK(panel_state_colour(s) != 0u,
              "%s has no colour", panel_state_name(s));
    }
}

static void test_red_is_reserved_for_danger(void)
{
    /* D-012 narrowed red to "danger and stop, only", citing IEC 60073, and the
     * panel had already broken that once by rendering a dead link in red
     * because the row felt bad. This pins the narrowing against the next
     * person who reaches for it. */
    const uint32_t red = panel_state_colour(PANEL_ST_DANGER);
    for (int i = 0; i < (int)PANEL_ST_COUNT; i++) {
        if (i == (int)PANEL_ST_DANGER) continue;
        CHECK(panel_state_colour((panel_state_t)i) != red,
              "%s is red, which D-012 reserves for danger",
              panel_state_name((panel_state_t)i));
    }
}

/* ---- the live path ----------------------------------------------------- */

static void test_link_down_is_offline_via_r2(void)
{
    panel_state_t st; panel_offline_mode_t m;
    panel_state_from_link(false, true, &st, &m);
    CHECK(st == PANEL_ST_OFFLINE, "link down gave %s", panel_state_name(st));
    CHECK(m == PANEL_OFF_R2,
          "link down blamed something other than the BLE link");
}

static void test_link_up_is_idle_and_nothing_richer(void)
{
    panel_state_t st; panel_offline_mode_t m;
    const uint32_t mask = panel_state_from_link(true, false, &st, &m);
    CHECK(st == PANEL_ST_IDLE, "link up gave %s", panel_state_name(st));
    /* The mask, not just the winner: `idle` would also be the answer if the
     * derivation had ALSO claimed `listen` and lost the contest. This board
     * has no microphone, so listen must never be a candidate in the first
     * place. */
    CHECK(mask == (1u << PANEL_ST_IDLE),
          "link up made a state other than idle a candidate");
}

static void test_transition_is_waking_and_stays_unranked(void)
{
    panel_state_t st; panel_offline_mode_t m;
    const uint32_t mask = panel_state_from_link(false, false, &st, &m);
    CHECK(st == PANEL_ST_WAKING, "a link transition gave %s",
          panel_state_name(st));
    /* Chosen deliberately, never resolved to. If `waking` were in the mask it
     * would be competing, which AC2b forbids. */
    CHECK(mask == 0u, "waking was put into the severity contest");
    CHECK(!panel_state_is_ranked(st), "waking became ranked");
}

static void test_offline_beats_idle_if_both_were_ever_set(void)
{
    /* The derivation makes them mutually exclusive today. This pins the
     * ordering anyway, because the day a second link arrives, "up on one and
     * down on another" is exactly the mask that appears -- and the panel must
     * report the fault, not the health. */
    CHECK(panel_state_resolve((1u << PANEL_ST_OFFLINE) | (1u << PANEL_ST_IDLE))
              == PANEL_ST_OFFLINE,
          "a healthy link masked a dead one");
}

static void test_null_outs_are_tolerated(void)
{
    /* A caller that wants only the mask must not have to invent storage. */
    panel_state_from_link(true, false, NULL, NULL);
    CHECK(1, "null out-params did not crash");
}

int main(void)
{
    test_unranked_states_report_minus_one();
    test_out_of_range_is_refused();
    test_the_nine_are_ranked_in_enum_order();
    test_unranked_cannot_beat_ranked();
    test_empty_mask_has_no_default();
    test_severity_order_holds_pairwise();
    test_wake_frame_fires_for_exactly_three();
    test_no_unranked_state_wakes();
    test_offline_modes_give_distinct_reasons();
    test_mode_is_ignored_by_every_other_state();
    test_every_state_renders();
    test_red_is_reserved_for_danger();
    test_link_down_is_offline_via_r2();
    test_link_up_is_idle_and_nothing_richer();
    test_transition_is_waking_and_stays_unranked();
    test_offline_beats_idle_if_both_were_ever_set();
    test_null_outs_are_tolerated();

    printf("%s: %d checks, %d failures\n",
           failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
