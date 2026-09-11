/* Host tests for the wake frame's decisions (#101 AC3).
 *
 * Illegal cases first in every group, for the reason the panel_state suite
 * gives: a test that only walks the legal cases passes with the guard deleted.
 * Here the illegal cases are a frame that fires for the wrong state, one that
 * re-fires on a state that merely holds, one that outlives its condition, and
 * a box that claims a number this board never measured. */
#include <stdio.h>
#include <string.h>

#include "panel_wake.h"

static int failures = 0, checks = 0;

#define CHECK(cond, ...) do {                                \
    checks++;                                                \
    if (!(cond)) { failures++;                               \
        printf("  FAIL: "); printf(__VA_ARGS__);             \
        printf("\n        at %s:%d\n", __FILE__, __LINE__); }\
} while (0)

#define T0 100000u

/* ---- when it fires ------------------------------------------------------ */

static void test_only_the_three_fire_it(void)
{
    /* A full sweep from a fresh tracker, every state, with the expectation
     * derived from AC3's list written out -- not from panel_state_wakes, which
     * would make this a test that the function agrees with itself. */
    for (int i = 0; i < (int)PANEL_ST_COUNT; i++) {
        const panel_state_t s = (panel_state_t)i;
        const bool expect = (s == PANEL_ST_DANGER || s == PANEL_ST_OFFLINE ||
                             s == PANEL_ST_ATTENTION);
        panel_wake_t w; panel_wake_init(&w);
        const bool up = panel_wake_step(&w, s, PANEL_OFF_R2, T0);
        CHECK(up == expect, "%s: wake frame %d, expected %d",
              panel_state_name(s), (int)up, (int)expect);
    }
}

static void test_waking_the_state_never_fires_it(void)
{
    /* The one this PR's sibling made common: `waking` now appears on every
     * reconnect. It is unranked by design (D-023) and must never interrupt
     * anyone -- the household would be woken to hear the panel reconnecting. */
    panel_wake_t w; panel_wake_init(&w);
    CHECK(!panel_wake_step(&w, PANEL_ST_WAKING, PANEL_OFF_COUNT, T0),
          "waking fired the wake frame");
    CHECK(!panel_wake_step(&w, PANEL_ST_IDLE, PANEL_OFF_COUNT, T0 + 3000),
          "a reconnect completing fired the wake frame");
}

/* ---- how long it lasts -------------------------------------------------- */

static void test_it_decays_at_six_seconds_and_stays_down(void)
{
    panel_wake_t w; panel_wake_init(&w);
    panel_wake_step(&w, PANEL_ST_OFFLINE, PANEL_OFF_R2, T0);
    CHECK(panel_wake_step(&w, PANEL_ST_OFFLINE, PANEL_OFF_R2,
                          T0 + PANEL_WAKE_DECAY_MS - 1),
          "down before 6 s");
    CHECK(!panel_wake_step(&w, PANEL_ST_OFFLINE, PANEL_OFF_R2,
                           T0 + PANEL_WAKE_DECAY_MS),
          "still up at 6 s");
    /* THE ILLEGAL CASE: holding the state must not bring it back. An hour of
     * offline is one interruption, not six hundred. */
    CHECK(!panel_wake_step(&w, PANEL_ST_OFFLINE, PANEL_OFF_R2, T0 + 3600000u),
          "re-fired while the state merely held");
    CHECK(PANEL_WAKE_DECAY_MS == 6000u, "decay is %u ms, the reference's is 6000",
          (unsigned)PANEL_WAKE_DECAY_MS);
}

static void test_leaving_the_state_takes_it_down_at_once(void)
{
    /* A wake frame outliving its condition announces a fault that has
     * cleared. R2 reconnecting 2 s into an OFFLINE frame must clear it in the
     * same step, not 4 s later. */
    panel_wake_t w; panel_wake_init(&w);
    panel_wake_step(&w, PANEL_ST_OFFLINE, PANEL_OFF_R2, T0);
    CHECK(!panel_wake_step(&w, PANEL_ST_IDLE, PANEL_OFF_COUNT, T0 + 2000),
          "the frame outlived the fault it was announcing");
}

static void test_it_decays_across_the_clock_wrap(void)
{
    panel_wake_t w; panel_wake_init(&w);
    const uint32_t t = 0xFFFFF000u;                 /* 4096 ms before the wrap */
    panel_wake_step(&w, PANEL_ST_DANGER, PANEL_OFF_COUNT, t);
    CHECK(panel_wake_step(&w, PANEL_ST_DANGER, PANEL_OFF_COUNT, t + 5000u),
          "down 5 s in, across the wrap");
    CHECK(!panel_wake_step(&w, PANEL_ST_DANGER, PANEL_OFF_COUNT, t + 6000u),
          "never decayed across the wrap");
}

/* ---- what re-fires it --------------------------------------------------- */

static void test_a_new_fault_is_a_new_interruption(void)
{
    panel_wake_t w; panel_wake_init(&w);
    panel_wake_step(&w, PANEL_ST_OFFLINE, PANEL_OFF_R2, T0);
    panel_wake_step(&w, PANEL_ST_OFFLINE, PANEL_OFF_R2, T0 + 7000);  /* decayed */

    /* Escalation. */
    CHECK(panel_wake_step(&w, PANEL_ST_DANGER, PANEL_OFF_COUNT, T0 + 8000),
          "offline -> danger did not re-fire");

    /* A different offline VIEW is a different broken thing. */
    panel_wake_init(&w);
    panel_wake_step(&w, PANEL_ST_OFFLINE, PANEL_OFF_R2, T0);
    panel_wake_step(&w, PANEL_ST_OFFLINE, PANEL_OFF_R2, T0 + 7000);
    CHECK(panel_wake_step(&w, PANEL_ST_OFFLINE, PANEL_OFF_LLM, T0 + 8000),
          "offline R2 -> offline LLM did not re-fire");

    /* And the same fault coming back after a recovery. */
    panel_wake_init(&w);
    panel_wake_step(&w, PANEL_ST_OFFLINE, PANEL_OFF_R2, T0);
    panel_wake_step(&w, PANEL_ST_IDLE, PANEL_OFF_COUNT, T0 + 1000);
    CHECK(panel_wake_step(&w, PANEL_ST_OFFLINE, PANEL_OFF_R2, T0 + 2000),
          "a second drop after a recovery did not re-fire");
}

static void test_a_tap_dismisses_until_the_state_changes(void)
{
    panel_wake_t w; panel_wake_init(&w);
    panel_wake_step(&w, PANEL_ST_OFFLINE, PANEL_OFF_R2, T0);
    panel_wake_dismiss(&w);
    CHECK(!panel_wake_showing(&w), "a tap did not dismiss");
    CHECK(!panel_wake_step(&w, PANEL_ST_OFFLINE, PANEL_OFF_R2, T0 + 100),
          "the next tick brought a dismissed frame back");
    /* Dismissing is not muting: the next fault still interrupts. */
    CHECK(panel_wake_step(&w, PANEL_ST_DANGER, PANEL_OFF_COUNT, T0 + 200),
          "a dismissal muted the next fault");
}

/* ---- the box ------------------------------------------------------------ */

static void test_only_offline_has_a_box(void)
{
    /* THE ILLEGAL CASE: danger and attention have boxes in the reference --
     * a fault code and a battery percent -- and this board measures neither.
     * The sweep proves no other state grows one. */
    for (int i = 0; i < (int)PANEL_ST_COUNT; i++) {
        const panel_state_t s = (panel_state_t)i;
        const char *subj = "untouched", *what = "untouched";
        const bool has = panel_wake_box(s, PANEL_OFF_R2, &subj, &what);
        CHECK(has == (s == PANEL_ST_OFFLINE), "%s: box %d",
              panel_state_name(s), (int)has);
        if (!has)
            CHECK(strcmp(subj, "untouched") == 0 && strcmp(what, "untouched") == 0,
                  "%s wrote a box it does not have", panel_state_name(s));
    }
    const char *subj = NULL, *what = NULL;
    CHECK(!panel_wake_box(PANEL_ST_OFFLINE, PANEL_OFF_COUNT, &subj, &what),
          "an out-of-range offline view produced a box");
    CHECK(!panel_wake_box(PANEL_ST_OFFLINE, (panel_offline_mode_t)-1, &subj, &what),
          "a negative offline view produced a box");
}

static void test_each_box_names_the_right_link(void)
{
    /* The cells, not their distinctness -- the lesson the offline reasons
     * already taught this repo, where swapping two strings survived a
     * pairwise-inequality test. */
    static const struct { panel_offline_mode_t m; const char *s, *w; } k[] = {
        { PANEL_OFF_R2,  "R2",  "BLE"  },
        { PANEL_OFF_NET, "NET", "LINK" },
        { PANEL_OFF_LLM, "LLM", "SVC"  },
    };
    for (unsigned i = 0; i < sizeof k / sizeof k[0]; i++) {
        const char *subj = NULL, *what = NULL;
        CHECK(panel_wake_box(PANEL_ST_OFFLINE, k[i].m, &subj, &what) &&
              strcmp(subj, k[i].s) == 0 && strcmp(what, k[i].w) == 0,
              "view %d boxed '%s'/'%s', want '%s'/'%s'", (int)k[i].m,
              subj ? subj : "(null)", what ? what : "(null)", k[i].s, k[i].w);
    }
    CHECK(panel_wake_box(PANEL_ST_OFFLINE, PANEL_OFF_R2, NULL, NULL),
          "null outs refused");
}

static void test_down_for_floors_to_the_largest_whole_unit(void)
{
    static const struct { uint32_t ms; const char *want; } k[] = {
        { 0u,           "DOWN 0S"  },
        { 4101u,        "DOWN 4S"  },   /* first tick past the bound: it fires */
        { 59999u,       "DOWN 59S" },
        { 60000u,       "DOWN 1M"  },
        { 119999u,      "DOWN 1M"  },   /* floored: 2M would claim time not passed */
        { 3599999u,     "DOWN 59M" },
        { 3600000u,     "DOWN 1H"  },
        { 86399999u,    "DOWN 23H" },
        { 86400000u,    "DOWN 1D"  },
        { 0xFFFFFFFFu,  "DOWN 49D" },
    };
    for (unsigned i = 0; i < sizeof k / sizeof k[0]; i++) {
        char buf[16];
        panel_wake_format_down(k[i].ms, buf, sizeof buf);
        CHECK(strcmp(buf, k[i].want) == 0, "%u ms -> '%s', want '%s'",
              (unsigned)k[i].ms, buf, k[i].want);
    }
    /* Truncates, never overruns, always terminates. */
    char tiny[5] = { 'x', 'x', 'x', 'x', 'x' };
    panel_wake_format_down(3600000u, tiny, 4);
    CHECK(tiny[3] == '\0' && tiny[4] == 'x', "a short buffer was overrun");
    panel_wake_format_down(1000u, NULL, 8);
    panel_wake_format_down(1000u, tiny, 0);
    CHECK(tiny[4] == 'x', "a zero-length buffer was written");
}

/* ---- the rising edge the renderer paints on --------------------------- */

static void test_it_reports_each_rise_once(void)
{
    /* THE ILLEGAL CASES FIRST: holding a state, or entering one that does not
     * wake, must never report a rise -- a rise is what repaints the frame and
     * restarts the sweep, and a spurious one would replay it every tick. */
    panel_wake_t w; panel_wake_init(&w);
    panel_wake_step(&w, PANEL_ST_IDLE, PANEL_OFF_COUNT, T0);
    CHECK(!panel_wake_take_fired(&w), "idle reported a rise");
    panel_wake_step(&w, PANEL_ST_WAKING, PANEL_OFF_COUNT, T0 + 10);
    CHECK(!panel_wake_take_fired(&w), "waking reported a rise");

    panel_wake_step(&w, PANEL_ST_OFFLINE, PANEL_OFF_R2, T0 + 20);
    CHECK(panel_wake_take_fired(&w), "entering offline did not report a rise");
    CHECK(!panel_wake_take_fired(&w), "a rise was reported twice");
    panel_wake_step(&w, PANEL_ST_OFFLINE, PANEL_OFF_R2, T0 + 60);
    CHECK(!panel_wake_take_fired(&w), "holding offline reported a rise");

    /* An escalation while it is up is a rise: the frame must be repainted
     * for the worse fault, not left saying the old one. */
    panel_wake_step(&w, PANEL_ST_DANGER, PANEL_OFF_COUNT, T0 + 100);
    CHECK(panel_wake_showing(&w) && panel_wake_take_fired(&w),
          "offline -> danger while up did not report a rise");

    /* A dismissed frame is not painted, even if its rise was never taken. */
    panel_wake_init(&w);
    panel_wake_step(&w, PANEL_ST_OFFLINE, PANEL_OFF_R2, T0);
    panel_wake_dismiss(&w);
    CHECK(!panel_wake_take_fired(&w), "a dismissed rise was still reported");
    CHECK(!panel_wake_take_fired(NULL), "a null tracker reported a rise");
}

static void test_nulls(void)
{
    panel_wake_init(NULL);
    CHECK(!panel_wake_step(NULL, PANEL_ST_DANGER, PANEL_OFF_COUNT, T0),
          "a null tracker reported a frame");
    panel_wake_dismiss(NULL);
    CHECK(!panel_wake_showing(NULL), "a null tracker is showing");
}

int main(void)
{
    test_only_the_three_fire_it();
    test_waking_the_state_never_fires_it();
    test_it_decays_at_six_seconds_and_stays_down();
    test_leaving_the_state_takes_it_down_at_once();
    test_it_decays_across_the_clock_wrap();
    test_a_new_fault_is_a_new_interruption();
    test_a_tap_dismisses_until_the_state_changes();
    test_only_offline_has_a_box();
    test_each_box_names_the_right_link();
    test_down_for_floors_to_the_largest_whole_unit();
    test_it_reports_each_rise_once();
    test_nulls();

    printf("%s: %d checks, %d failures\n",
           failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
