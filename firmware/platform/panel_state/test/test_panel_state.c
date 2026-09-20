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

static void test_the_rank_is_d017s_order_by_name(void)
{
    /* THE ONE TEST THAT IS NOT ALLOWED TO CONSULT THE ENUM.
     *
     * Every other rank check derives its expectation from enum order, so all
     * of them are tautologies with respect to the ordering itself: a reviewer
     * reordered the nine states so that `thinking` outranked `offline` and all
     * 226 checks passed. The severity rank is this module's central claim and
     * it was pinned by nothing.
     *
     * D-017's rank as amended by D-031, transcribed from the ADRs and NOT
     * from the header:
     *   danger > offline > attention > misheard > waiting > answering >
     *   thinking > listen > idle > sleep
     * Written as names, because a list of enum constants would just be the
     * enum again in a different order of appearance. */
    static const char *const d017[] = {
        "danger", "offline", "attention", "misheard", "waiting",
        "answering", "thinking", "listen", "idle", "sleep",
    };
    const int n = (int)(sizeof d017 / sizeof d017[0]);
    CHECK(n == (int)PANEL_ST_RANKED_COUNT,
          "D-017/D-031 rank %d states, the enum ranks %d",
          n, (int)PANEL_ST_RANKED_COUNT);

    for (int i = 0; i < n && i < (int)PANEL_ST_RANKED_COUNT; i++)
        CHECK(strcmp(panel_state_name((panel_state_t)i), d017[i]) == 0,
              "rank %d is %s, D-017/D-031 say %s",
              i, panel_state_name((panel_state_t)i), d017[i]);
}

static void test_the_ten_are_ranked_in_enum_order(void)
{
    CHECK((int)PANEL_ST_RANKED_COUNT == 10,
          "the rank is a CLOSED ten-state set (D-017, amended by D-031); found %d",
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
    /* All ten at once must give danger, which is the case that matters. */
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

static void test_waking_is_bounded(void)
{
    /* THE ILLEGAL CASES FIRST. A dropped link is not `offline` the instant it
     * drops: r2_link sets DOWN and restarts the scan in one callback, so a
     * derivation keyed on the drop either flickers OFFLINE for a frame on
     * every reconnect or, keyed on DOWN being held, never says offline at all
     * -- which is what shipped. */
    panel_state_t st; panel_offline_mode_t m;
    panel_state_from_link(false, 0, PANEL_ST_COUNT, &st, &m);
    CHECK(st == PANEL_ST_WAKING, "a link that just dropped read %s",
          panel_state_name(st));
    panel_state_from_link(false, PANEL_WAKING_BOUND_MS, PANEL_ST_COUNT, &st, &m);
    CHECK(st == PANEL_ST_WAKING,
          "a reconnect exactly at the bound read %s -- the bound is inclusive",
          panel_state_name(st));
    CHECK(m == PANEL_OFF_COUNT, "waking left an offline view set");

    /* One millisecond past it, he is gone. */
    panel_state_from_link(false, PANEL_WAKING_BOUND_MS + 1, PANEL_ST_COUNT, &st, &m);
    CHECK(st == PANEL_ST_OFFLINE, "1 ms past the bound read %s",
          panel_state_name(st));

    /* And it STAYS offline however long he is away: nothing past the bound is
     * allowed to fall back into waking, including a count near the top of
     * the range. */
    panel_state_from_link(false, 0xFFFFFFFFu, PANEL_ST_COUNT, &st, &m);
    CHECK(st == PANEL_ST_OFFLINE, "gone for 49 days read %s",
          panel_state_name(st));

    /* Pinned as a range rather than a value. Below 3650 ms a reconnect that
     * misses two ~500 ms scan windows from the 2650 ms base (six of P2's eight
     * samples sit at 2600-2650) calls a false OFFLINE, which P2's own write-up
     * warns 8 trials could not see; far above it, a real absence goes
     * unannounced for longer than the evidence justifies. */
    CHECK(PANEL_WAKING_BOUND_MS >= 3650u && PANEL_WAKING_BOUND_MS <= 5000u,
          "the waking bound is %u ms, outside P2's evidence",
          (unsigned)PANEL_WAKING_BOUND_MS);
}

static void test_a_live_link_is_never_offline(void)
{
    /* A caller passing a stale count beside a live link must not be able to
     * call him offline. Up wins, whatever the clock says. */
    panel_state_t st; panel_offline_mode_t m;
    panel_state_from_link(true, 0xFFFFFFFFu, PANEL_ST_COUNT, &st, &m);
    CHECK(st == PANEL_ST_IDLE, "a live link with a stale count read %s",
          panel_state_name(st));
    CHECK(m == PANEL_OFF_COUNT, "a live link left an offline view set");
}

static void test_waking_progress_saturates_and_never_wraps(void)
{
    /* The ILLEGAL case: 4,294,968 ms (about 72 minutes) is where
     * ms * 1000 overflows 32 bits. A multiply-first bar would snap back to
     * near-empty an hour into his absence. */
    CHECK(panel_state_waking_permille(4294968u) == 1000u,
          "the bar wrapped at the 32-bit overflow: %u",
          panel_state_waking_permille(4294968u));
    CHECK(panel_state_waking_permille(0xFFFFFFFFu) == 1000u,
          "the bar wrapped at the top of the range: %u",
          panel_state_waking_permille(0xFFFFFFFFu));

    CHECK(panel_state_waking_permille(0) == 0u, "the bar starts non-empty");
    CHECK(panel_state_waking_permille(PANEL_WAKING_BOUND_MS / 2) == 500u,
          "halfway reads %u", panel_state_waking_permille(PANEL_WAKING_BOUND_MS / 2));
    CHECK(panel_state_waking_permille(PANEL_WAKING_BOUND_MS) == 1000u,
          "the bar is not full at the bound");

    /* Monotonic across the whole bound: a bar that ever steps backwards reads
     * as the reconnect starting over. */
    unsigned prev = 0;
    for (uint32_t ms = 0; ms <= PANEL_WAKING_BOUND_MS + 50; ms += 7) {
        const unsigned p = panel_state_waking_permille(ms);
        CHECK(p >= prev && p <= 1000u, "bar went %u -> %u at %u ms", prev, p, ms);
        prev = p;
    }
}

static void test_past_the_bound_is_offline_via_r2(void)
{
    panel_state_t st; panel_offline_mode_t m;
    panel_state_from_link(false, PANEL_WAKING_BOUND_MS + 1, PANEL_ST_COUNT, &st, &m);
    CHECK(st == PANEL_ST_OFFLINE, "past the bound gave %s", panel_state_name(st));
    CHECK(m == PANEL_OFF_R2,
          "an unreachable R2 blamed something other than the BLE link");
    /* Through the mode, to the string the operator actually reads. Asserting
     * the enum alone leaves the mapping from mode to reason untested on the
     * live path. */
    CHECK(strcmp(panel_state_since(st, m), "R2 LINK DOWN") == 0,
          "the live offline path reads '%s'", panel_state_since(st, m));
}

static void test_link_up_is_idle_and_nothing_richer(void)
{
    panel_state_t st; panel_offline_mode_t m;
    const uint32_t mask = panel_state_from_link(true, 0, PANEL_ST_COUNT, &st, &m);
    CHECK(st == PANEL_ST_IDLE, "link up gave %s", panel_state_name(st));
    /* The mask, not just the winner: `idle` would also be the answer if the
     * derivation had ALSO claimed `listen` and lost the contest. A caller
     * that passes PANEL_ST_COUNT -- no live exchange -- must get exactly
     * `idle` as a candidate, nothing else. */
    CHECK(mask == (1u << PANEL_ST_IDLE),
          "no voice state still made something other than idle a candidate");
    /* The mode matters even when there is no fault: panel_ui treats it as part
     * of the repaint identity, so a stale PANEL_OFF_R2 written here would
     * decide whether a later transition repaints at all. */
    CHECK(m == PANEL_OFF_COUNT, "link up left an offline view set");
}

static void test_voice_state_wins_over_idle_while_link_is_up(void)
{
    /* E2E v0 slice 3.2: LISTEN, THINKING and MISHEARD are real states now,
     * each strictly more severe than idle in D-017's rank, so any one of
     * them present must win the contest, not merely be offered and lose. */
    const panel_state_t voices[] = {
        PANEL_ST_LISTEN, PANEL_ST_THINKING, PANEL_ST_MISHEARD,
        PANEL_ST_ANSWERING,
    };
    for (size_t i = 0; i < sizeof voices / sizeof voices[0]; i++) {
        panel_state_t st; panel_offline_mode_t m;
        const uint32_t mask = panel_state_from_link(true, 0, voices[i], &st, &m);
        CHECK(st == voices[i], "voice state %s lost to %s",
              panel_state_name(voices[i]), panel_state_name(st));
        CHECK(mask == ((1u << PANEL_ST_IDLE) | (1u << voices[i])),
              "voice state %s changed which states were candidates",
              panel_state_name(voices[i]));
    }
}

static void test_voice_state_is_ignored_while_the_link_is_down(void)
{
    /* A hold cannot start without `awake` (panel_face.h rule 6), so a real
     * caller never has a voice state to report here -- but a stale one must
     * not silently outrank a real fault if it is ever passed anyway. */
    panel_state_t st; panel_offline_mode_t m;
    const uint32_t mask = panel_state_from_link(false, PANEL_WAKING_BOUND_MS + 1,
                                                PANEL_ST_LISTEN, &st, &m);
    CHECK(st == PANEL_ST_OFFLINE, "a stale voice state beat a real fault: %s",
          panel_state_name(st));
    CHECK(mask == (1u << PANEL_ST_OFFLINE),
          "a stale voice state was offered as a candidate while offline");
}

static void test_out_of_range_voice_state_reports_nothing(void)
{
    /* PANEL_ST_COUNT is the documented "no live exchange" value, and
     * anything else out of the ranked set (RELEASED, WAKING, UNPROVISIONED,
     * or plain garbage) must be refused the same way -- a caller's bug must
     * not become a state on the glass. */
    panel_state_t st; panel_offline_mode_t m;
    const uint32_t mask = panel_state_from_link(true, 0, PANEL_ST_COUNT, &st, &m);
    CHECK(mask == (1u << PANEL_ST_IDLE), "PANEL_ST_COUNT was treated as a state");

    const uint32_t mask2 = panel_state_from_link(true, 0, PANEL_ST_RELEASED,
                                                 &st, &m);
    CHECK(mask2 == (1u << PANEL_ST_IDLE),
          "an unranked voice state (RELEASED) was offered as a candidate");
}

static void test_released_ignores_a_live_voice_state(void)
{
    /* GOODNIGHT wins over an exchange in progress (panel_face.h precedence
     * #4); this is the other half -- once `released` is true, whatever the
     * caller still passes for voice_state must not resurrect the exchange
     * on the glass. Retiring it is the caller's job (panel_exchange_retire),
     * not this function's to notice. */
    panel_state_t st; panel_offline_mode_t m;
    const uint32_t mask = panel_state_from_power(true, true, 0, PANEL_ST_LISTEN,
                                                 &st, &m);
    CHECK(st == PANEL_ST_RELEASED, "released lost to a stale voice state: %s",
          panel_state_name(st));
    CHECK(mask == 0u, "released left a voice state in the candidate mask");
}

static void test_transition_is_waking_and_stays_unranked(void)
{
    panel_state_t st; panel_offline_mode_t m;
    const uint32_t mask = panel_state_from_link(false, 1000, PANEL_ST_COUNT, &st, &m);
    CHECK(st == PANEL_ST_WAKING, "a link transition gave %s",
          panel_state_name(st));
    /* Chosen deliberately, never resolved to. If `waking` were in the mask it
     * would be competing, which AC2b forbids. */
    CHECK(mask == 0u, "waking was put into the severity contest");
    CHECK(m == PANEL_OFF_COUNT, "a link transition left an offline view set");
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
    panel_state_from_link(true, 0, PANEL_ST_COUNT, NULL, NULL);
    panel_state_from_link(false, PANEL_WAKING_BOUND_MS + 1, PANEL_ST_COUNT, NULL, NULL);
    CHECK(1, "null out-params did not crash");
}

/* ---- the cells, not just their shape ----------------------------------- */

static void test_each_offline_view_blames_the_right_thing(void)
{
    /* Pairwise inequality was NOT enough, proven by mutation: swapping the R2
     * and NET reasons survived all 193 checks, and the panel would have
     * printed "NO INTERNET" for a dropped BLE link on a build with no network
     * stack at all. The three views ARE the justification for one offline
     * state, so their content is the property, not their distinctness. */
    CHECK(strcmp(panel_state_since(PANEL_ST_OFFLINE, PANEL_OFF_R2),
                 "R2 LINK DOWN") == 0,
          "the R2 view says '%s'",
          panel_state_since(PANEL_ST_OFFLINE, PANEL_OFF_R2));
    CHECK(strcmp(panel_state_since(PANEL_ST_OFFLINE, PANEL_OFF_NET),
                 "NO INTERNET") == 0,
          "the NET view says '%s'",
          panel_state_since(PANEL_ST_OFFLINE, PANEL_OFF_NET));
    CHECK(strcmp(panel_state_since(PANEL_ST_OFFLINE, PANEL_OFF_LLM),
                 "LLM DOWN") == 0,
          "the LLM view says '%s'",
          panel_state_since(PANEL_ST_OFFLINE, PANEL_OFF_LLM));
}

static void test_the_state_is_listen_and_the_word_is_listening(void)
{
    /* The one documented divergence between a state id and its rendering,
     * called out in the header and the table -- and untested until a mutation
     * shortened the word and nothing failed. Documented is not covered. */
    CHECK(strcmp(panel_state_name(PANEL_ST_LISTEN), "listen") == 0,
          "the state id drifted from `listen`");
    CHECK(strcmp(panel_state_word(PANEL_ST_LISTEN), "LISTENING") == 0,
          "the word on the glass drifted from LISTENING");
}

static void test_colour_partitions_by_meaning(void)
{
    /* By SEMANTIC CLASS, not by hex. Asserting values would pin the palette,
     * which D-012 Amendment A explicitly leaves to the medium; asserting the
     * partition pins what a colour MEANS, which is the half that must not
     * fork. A state moving between meanings fails here -- `waiting` turning
     * amber would promote a patient wait to needs-monitoring, and that
     * mutation survived the suite before this test existed. */
    const panel_state_t amber[] = { PANEL_ST_OFFLINE, PANEL_ST_ATTENTION,
                                    PANEL_ST_MISHEARD };
    const panel_state_t cyan[]  = { PANEL_ST_ANSWERING, PANEL_ST_THINKING, PANEL_ST_LISTEN };
    const panel_state_t blue[]  = { PANEL_ST_WAITING, PANEL_ST_WAKING,
                                    PANEL_ST_UNPROVISIONED };
    const panel_state_t rest[]  = { PANEL_ST_SLEEP, PANEL_ST_RELEASED };

    struct { const char *what; const panel_state_t *set; unsigned n; uint32_t c; }
    group[] = {
        { "amber/needs-monitoring", amber, 3, PANEL_C_AMBER },
        { "cyan/engaged",           cyan,  3, PANEL_C_CYAN  },
        { "blue/neutral",           blue,  3, PANEL_C_BLUE  },
        { "magenta/rest",           rest,  2, PANEL_C_MAGENTA },
    };

    /* A BITMASK, not a count. Counting entries only proves "at least one
     * class": a state listed in two groups while another is listed in none
     * sums to the right total and passes, which is how a state could end up
     * unchecked while the completeness check reported success. */
    uint32_t seen = 0;
    for (unsigned g = 0; g < sizeof group / sizeof group[0]; g++) {
        for (unsigned i = 0; i < group[g].n; i++) {
            const panel_state_t s = group[g].set[i];
            CHECK(panel_state_colour(s) == group[g].c,
                  "%s is not %s", panel_state_name(s), group[g].what);
            CHECK((seen & (1u << s)) == 0,
                  "%s is in two colour classes", panel_state_name(s));
            seen |= 1u << s;
        }
    }
    CHECK(panel_state_colour(PANEL_ST_DANGER) == PANEL_C_RED, "danger is not red");
    CHECK(panel_state_colour(PANEL_ST_IDLE) == PANEL_C_GREEN, "idle is not green");
    seen |= (1u << PANEL_ST_DANGER) | (1u << PANEL_ST_IDLE);

    const uint32_t all = (1u << PANEL_ST_COUNT) - 1u;
    CHECK(seen == all, "states in no colour class: mask 0x%X", (all & ~seen));
}

static void test_answering_is_its_own_row_and_never_wakes(void)
{
    CHECK(panel_state_lights(PANEL_ST_ANSWERING) != R2L_LISTEN,
          "answering drove listen's lights; the two share a PSI colour and "
          "differ only on the holo and logic");
    CHECK(panel_state_lights(PANEL_ST_ANSWERING) == R2L_ANSWERING,
          "answering does not drive its own light row");
    CHECK(!panel_state_wakes(PANEL_ST_ANSWERING),
          "answering fired the wake frame; a reply is not a severity");
    CHECK(panel_state_resolve((1u << PANEL_ST_ANSWERING) |
                              (1u << PANEL_ST_THINKING)) == PANEL_ST_ANSWERING,
          "thinking outranked answering (D-031: waiting > answering > thinking)");
    CHECK(panel_state_resolve((1u << PANEL_ST_ANSWERING) |
                              (1u << PANEL_ST_WAITING)) == PANEL_ST_WAITING,
          "answering outranked waiting (D-031: waiting > answering)");
    CHECK(strcmp(panel_state_word(PANEL_ST_ANSWERING), "ANSWERING") == 0,
          "answering's word is not ANSWERING");
}

static void test_rest_is_not_the_no_claim_colour(void)
{
    /* D-012 Amendment A gives the panel a neutral grey for "we cannot say" and
     * says in terms that it is "deliberately not a colour claim: it is the
     * absence of one". `sleep` and `released` are positive claims about a
     * deliberate act, so they must not be drawn in it -- they were, and it took
     * a reviewer noticing the hex matched V5_LABEL byte for byte. */
    const uint32_t no_claim = 0x7C8A8D;
    for (int i = 0; i < (int)PANEL_ST_COUNT; i++)
        CHECK(panel_state_colour((panel_state_t)i) != no_claim,
              "%s renders in the panel's absence-of-a-claim colour",
              panel_state_name((panel_state_t)i));
}

/* ---- GOODNIGHT (D-023, E2E v0 slice 1) ---------------------------------- */

static void test_released_is_shown_whatever_the_link_says(void)
{
    panel_state_t st; panel_offline_mode_t m;
    /* Long gone: without the flag this is OFFLINE, a fault. */
    panel_state_from_power(true, false, 0xFFFFFFFFu, PANEL_ST_COUNT, &st, &m);
    CHECK(st == PANEL_ST_RELEASED, "a long release read as %d", (int)st);
    CHECK(m == PANEL_OFF_COUNT, "released carried an offline mode");
    /* Still up while the teardown runs: the claim is about the tap. */
    panel_state_from_power(true, true, 0, PANEL_ST_COUNT, &st, &m);
    CHECK(st == PANEL_ST_RELEASED, "released showed the link instead");
}

static void test_released_raises_no_ranked_state(void)
{
    panel_state_t st; panel_offline_mode_t m;
    const uint32_t mask = panel_state_from_power(true, false, 0xFFFFFFFFu, PANEL_ST_COUNT, &st, &m);
    CHECK(mask == 0, "released set ranked bits 0x%x", (unsigned)mask);
    CHECK(!panel_state_wakes(st), "released would wake the household");
    CHECK(!panel_state_is_ranked(st), "released entered the severity order");
}

static void test_not_released_is_exactly_from_link(void)
{
    const uint32_t away[] = { 0, PANEL_WAKING_BOUND_MS, PANEL_WAKING_BOUND_MS + 1, 0xFFFFFFFFu };
    for (int up = 0; up < 2; up++)
        for (unsigned i = 0; i < sizeof away / sizeof away[0]; i++) {
            panel_state_t a, b; panel_offline_mode_t ma, mb;
            const uint32_t ka = panel_state_from_power(false, up, away[i], PANEL_ST_COUNT, &a, &ma);
            const uint32_t kb = panel_state_from_link(up, away[i], PANEL_ST_COUNT, &b, &mb);
            CHECK(ka == kb && a == b && ma == mb,
                  "from_power diverged from from_link (up=%d away=%u)", up, (unsigned)away[i]);
        }
}

/* ---- the same state on his body (E2E v0 slice 2) ------------------------ */

static void test_lights_refuse_what_has_no_link(void)
{
    /* ILLEGAL FIRST. A row for WAKING would be written over a link that is
     * not up; a row for junk would be a guess. */
    CHECK(panel_state_lights(PANEL_ST_WAKING) == R2L_NONE, "WAKING must light nothing");
    CHECK(panel_state_lights(PANEL_ST_UNPROVISIONED) == R2L_NONE,
          "UNPROVISIONED must light nothing");
    CHECK(panel_state_lights(PANEL_ST_COUNT) == R2L_NONE, "COUNT must light nothing");
    CHECK(panel_state_lights((panel_state_t)-1) == R2L_NONE, "-1 must light nothing");
    CHECK(panel_state_lights((panel_state_t)99) == R2L_NONE, "99 must light nothing");
    CHECK(panel_state_lights(PANEL_ST_RELEASED) == R2L_NONE,
          "RELEASED must light nothing: GOODNIGHT writes its own row, then revokes");
}

static void test_every_ranked_state_lights_its_own_row(void)
{
    /* By NAME, against the lights module's own names -- so a row swapped in
     * the switch (IDLE lighting LISTEN) fails here, which a test that only
     * asserted "some row" would pass. */
    for (int s = 0; s < PANEL_ST_RANKED_COUNT; s++) {
        const r2_lights_state_t l = panel_state_lights((panel_state_t)s);
        CHECK(strcmp(panel_state_name((panel_state_t)s), r2_lights_name(l)) == 0,
              "%s lights the %s row", panel_state_name((panel_state_t)s), r2_lights_name(l));
    }
}

int main(void)
{
    test_unranked_states_report_minus_one();
    test_out_of_range_is_refused();
    test_the_rank_is_d017s_order_by_name();
    test_the_ten_are_ranked_in_enum_order();
    test_answering_is_its_own_row_and_never_wakes();
    test_unranked_cannot_beat_ranked();
    test_empty_mask_has_no_default();
    test_severity_order_holds_pairwise();
    test_wake_frame_fires_for_exactly_three();
    test_no_unranked_state_wakes();
    test_offline_modes_give_distinct_reasons();
    test_mode_is_ignored_by_every_other_state();
    test_every_state_renders();
    test_red_is_reserved_for_danger();
    test_waking_is_bounded();
    test_a_live_link_is_never_offline();
    test_waking_progress_saturates_and_never_wraps();
    test_past_the_bound_is_offline_via_r2();
    test_link_up_is_idle_and_nothing_richer();
    test_voice_state_wins_over_idle_while_link_is_up();
    test_voice_state_is_ignored_while_the_link_is_down();
    test_out_of_range_voice_state_reports_nothing();
    test_released_ignores_a_live_voice_state();
    test_transition_is_waking_and_stays_unranked();
    test_offline_beats_idle_if_both_were_ever_set();
    test_null_outs_are_tolerated();
    test_each_offline_view_blames_the_right_thing();
    test_the_state_is_listen_and_the_word_is_listening();
    test_colour_partitions_by_meaning();
    test_rest_is_not_the_no_claim_colour();
    test_released_is_shown_whatever_the_link_says();
    test_released_raises_no_ranked_state();
    test_not_released_is_exactly_from_link();
    test_lights_refuse_what_has_no_link();
    test_every_ranked_state_lights_its_own_row();

    printf("%s: %d checks, %d failures\n",
           failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
