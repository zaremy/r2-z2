/* What a press meant. The illegal cases first, then the operator's own
 * gestures replayed from the capture that found the bug. */
#include "panel_gesture.h"

#include <stdio.h>
#include <string.h>

static unsigned checks, failures;

#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        checks++;                                                           \
        if (!(cond)) {                                                      \
            failures++;                                                     \
            printf("  FAIL: ");                                             \
            printf(__VA_ARGS__);                                            \
            printf("\n        at %s:%d\n", __FILE__, __LINE__);             \
        }                                                                   \
    } while (0)

static const char *name(panel_gesture_t g)
{
    switch (g) {
    case PANEL_GESTURE_NONE:        return "NONE";
    case PANEL_GESTURE_TAP:         return "TAP";
    case PANEL_GESTURE_SWIPE_LEFT:  return "SWIPE_LEFT";
    case PANEL_GESTURE_SWIPE_RIGHT: return "SWIPE_RIGHT";
    }
    return "?";
}

/* A press built from the two ends of a real trace, the way panel_touch
 * accumulates one. max_dev defaults to the larger travel, which is what a
 * straight drag produces. */
static panel_press_t press(int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                           uint32_t samples)
{
    panel_press_t p;
    memset(&p, 0, sizeof p);
    p.press_x = x0; p.press_y = y0;
    p.last_x = x1;  p.last_y = y1;
    p.samples = samples;
    const int32_t ax = x1 > x0 ? x1 - x0 : x0 - x1;
    const int32_t ay = y1 > y0 ? y1 - y0 : y0 - y1;
    p.max_dev = ax > ay ? ax : ay;
    return p;
}

/* ---- THE BUG ITSELF ------------------------------------------------------ */

static void test_a_press_seen_once_is_never_a_tap(void)
{
    /* THE ILLEGAL CASE, and it is the whole finding. A flick caught by a
     * single poll reports itself as motionless -- press position IS last
     * position -- so every distance is zero and the old chain fell through to
     * "moved less than PANEL_TAP_PX" and fired a TAP at wherever the finger
     * happened to be mid-flight. On SERVICE that opens a row nobody chose. On
     * the ladder it is a tap on RUN. */
    panel_press_t p = press(136, 275, 136, 275, 1);
    CHECK(panel_gesture_classify(&p) == PANEL_GESTURE_NONE,
          "a once-seen press became %s", name(panel_gesture_classify(&p)));

    /* Not even when it is a perfectly plausible tap position. One sample
     * cannot distinguish a still finger from a fast one; the panel does
     * nothing rather than guess which. */
    p = press(200, 300, 200, 300, 1);
    CHECK(panel_gesture_classify(&p) == PANEL_GESTURE_NONE,
          "a once-seen press in the middle of the glass became a tap");

    /* Zero samples is the same answer -- a press the poll never saw at all. */
    p = press(0, 0, 0, 0, 0);
    CHECK(panel_gesture_classify(&p) == PANEL_GESTURE_NONE,
          "a press with no samples produced a gesture");

    /* And the moment there are two, a still finger IS a tap again. The fix
     * must not cost the panel its taps. */
    p = press(200, 300, 200, 300, 2);
    CHECK(panel_gesture_classify(&p) == PANEL_GESTURE_TAP,
          "a twice-seen still press was not a tap");
}

static void test_a_voided_press_means_nothing(void)
{
    /* A finger already down when a frame rose under it must not dismiss what
     * it never saw -- however clean the gesture looks. */
    panel_press_t p = press(367, 230, 141, 267, 7);
    p.voided = true;
    CHECK(panel_gesture_classify(&p) == PANEL_GESTURE_NONE,
          "a voided swipe still fired");
    p = press(200, 300, 200, 300, 4);
    p.voided = true;
    CHECK(panel_gesture_classify(&p) == PANEL_GESTURE_NONE,
          "a voided tap still fired");
    CHECK(panel_gesture_classify(NULL) == PANEL_GESTURE_NONE, "NULL fired");
}

/* ---- direction and the thresholds --------------------------------------- */

static void test_the_swipe_thresholds(void)
{
    /* Exactly at the threshold counts -- the comparison is <= / >=. */
    panel_press_t p = press(200, 300, 200 - PANEL_SWIPE_PX, 300, 3);
    CHECK(panel_gesture_classify(&p) == PANEL_GESTURE_SWIPE_LEFT,
          "exactly %d px left was not a swipe", PANEL_SWIPE_PX);
    p = press(100, 300, 100 + PANEL_SWIPE_PX, 300, 3);
    CHECK(panel_gesture_classify(&p) == PANEL_GESTURE_SWIPE_RIGHT,
          "exactly %d px right was not a swipe", PANEL_SWIPE_PX);

    /* One pixel short is NOT a swipe, and is far enough to not be a tap
     * either: the dead gap is deliberate. BOTH directions -- the rightward
     * threshold had no test at all until a mutation battery deleted it and
     * every check stayed green, which is the same hole the operator's capture
     * hinted at when not one right swipe appeared in thirteen gestures. */
    p = press(200, 300, 200 - (PANEL_SWIPE_PX - 1), 300, 3);
    CHECK(panel_gesture_classify(&p) == PANEL_GESTURE_NONE,
          "one pixel short of the LEFT threshold still fired");
    p = press(100, 300, 100 + (PANEL_SWIPE_PX - 1), 300, 3);
    CHECK(panel_gesture_classify(&p) == PANEL_GESTURE_NONE,
          "one pixel short of the RIGHT threshold still fired");

    /* And a rightward drag inside the tap radius is a TAP, not a tiny swipe:
     * the two thresholds must not meet. */
    p = press(100, 300, 100 + (PANEL_TAP_PX - 1), 300, 3);
    CHECK(panel_gesture_classify(&p) == PANEL_GESTURE_TAP,
          "a %d px rightward drag was not a tap", PANEL_TAP_PX - 1);
}

static void test_the_thresholds_are_the_values_we_think(void)
{
    /* LITERAL PIXELS, ONCE. Every other test writes its fixtures relative to
     * the constants -- press(200, 300, 200 - PANEL_SWIPE_PX, ...) -- which
     * pins the comparisons and leaves the VALUES free: a mutation battery
     * changed 60 to 50 and 24 to 40 with the whole suite green. These are the
     * numbers a finger actually meets. */
    panel_press_t p = press(200, 300, 140, 300, 3);      /* exactly 60 left */
    CHECK(panel_gesture_classify(&p) == PANEL_GESTURE_SWIPE_LEFT,
          "60 px is no longer the swipe threshold");
    p = press(200, 300, 141, 300, 3);                    /* 59 */
    CHECK(panel_gesture_classify(&p) == PANEL_GESTURE_NONE,
          "59 px crossed the swipe threshold");
    p = press(200, 300, 223, 300, 3);                    /* 23: still a tap */
    CHECK(panel_gesture_classify(&p) == PANEL_GESTURE_TAP,
          "23 px is no longer inside the tap radius");
    p = press(200, 300, 224, 300, 3);                    /* 24: not a tap */
    CHECK(panel_gesture_classify(&p) == PANEL_GESTURE_NONE,
          "24 px is still inside the tap radius");
}

static void test_a_swipe_must_be_horizontal(void)
{
    /* Far enough across, but the finger arced: twice as far across as down is
     * the rule, and scrolling the SERVICE list is why. */
    panel_press_t p = press(367, 341, 243, 199, 2);   /* the operator's own */
    CHECK(panel_gesture_classify(&p) == PANEL_GESTURE_NONE,
          "a 124px-across, 142px-down drag was read as %s",
          name(panel_gesture_classify(&p)));

    /* Just over twice as far across passes; exactly twice does not, because
     * the test is a strict `adx > 2 * ady`. */
    p = press(200, 300, 100, 349, 3);        /* 100 across, 49 down */
    CHECK(panel_gesture_classify(&p) == PANEL_GESTURE_SWIPE_LEFT,
          "100 across / 49 down was not a swipe");
    p = press(200, 300, 100, 350, 3);        /* 100 across, 50 down */
    CHECK(panel_gesture_classify(&p) == PANEL_GESTURE_NONE,
          "100 across / 50 down was read as a swipe");
}

static void test_a_drag_out_and_back_is_not_a_tap(void)
{
    /* Ends where it started, but it scrolled the list on the way. The stray
     * distance is what says so, not the endpoints. */
    panel_press_t p = press(200, 300, 200, 300, 8);
    p.max_dev = 90;
    CHECK(panel_gesture_classify(&p) == PANEL_GESTURE_NONE,
          "a press that strayed 90px and returned was called a tap");

    p.max_dev = PANEL_TAP_PX;               /* exactly at the limit: not a tap */
    CHECK(panel_gesture_classify(&p) == PANEL_GESTURE_NONE,
          "a press that strayed exactly %d px was called a tap", PANEL_TAP_PX);
    p.max_dev = PANEL_TAP_PX - 1;
    CHECK(panel_gesture_classify(&p) == PANEL_GESTURE_TAP,
          "a press that strayed %d px was not a tap", PANEL_TAP_PX - 1);
}

/* ---- the operator's own gestures, replayed ------------------------------ */

static void test_the_captured_gestures_classify_as_measured(void)
{
    /* THE OPERATOR'S OWN PRESSES, and where the panel logged a verdict at the
     * time, that verdict is the expectation -- ground truth from the same file,
     * not something recomputed here. Re-derive with tools/seg_touch.py against
     * results/touch-sampling-before-2026-09-14.txt.
     *
     * The five that worked must still work: a fix that cured the flicks by
     * breaking the swipes would pass every test above this one. */
    struct { int32_t x0, y0, x1, y1; uint32_t n; panel_gesture_t want;
             const char *panel_said; } k[] = {
        /* --- the panel logged a verdict for these; it is the expectation --- */
        { 367, 230, 141, 267,  7, PANEL_GESTURE_SWIPE_LEFT,  "swipe left" },
        {   1, 275, 136, 275,  3, PANEL_GESTURE_SWIPE_RIGHT, "swipe right" },
        { 362, 291, 262, 281,  4, PANEL_GESTURE_SWIPE_LEFT,  "swipe left" },
        {   1, 282, 254, 275,  5, PANEL_GESTURE_SWIPE_RIGHT, "swipe right" },
        {   1, 204,  88, 215,  3, PANEL_GESTURE_SWIPE_RIGHT, "swipe right" },
        /* TWO SAMPLES, 271 px, and the panel called it correctly. This is the
         * press that says the floor is 2 and not 3. */
        { 358, 280,  87, 253,  2, PANEL_GESTURE_SWIPE_LEFT,  "swipe left" },
        /* --- the bug: one sample, and the panel acted on it --- */
        { 271, 318, 271, 318,  1, PANEL_GESTURE_NONE, "tap -> HARDWARE TEST" },
        {  59, 279,  59, 279,  1, PANEL_GESTURE_NONE, "tap on a LOCKED rung" },
        { 285, 300, 285, 300,  1, PANEL_GESTURE_NONE, "tap -> DIAGNOSTICS" },
        /* the other five 1-sample presses: taps that landed on nothing, which
         * is luck rather than design */
        {  53,  18,  53,  18,  1, PANEL_GESTURE_NONE, NULL },
        { 345, 299, 345, 299,  1, PANEL_GESTURE_NONE, NULL },
        {   1, 264,   1, 264,  1, PANEL_GESTURE_NONE, NULL },
        {  11, 284,  11, 284,  1, PANEL_GESTURE_NONE, NULL },
        {  36,  33,  36,  33,  1, PANEL_GESTURE_NONE, NULL },
        /* --- correctly refused: far across, but the finger arced --- */
        { 367, 341,  80, 166,  3, PANEL_GESTURE_NONE, NULL },
        /* --- long deliberate swipes, inside an interior so the panel logged
         *     no page change; the classifier must still read them --- */
        { 360, 250, 138, 263, 15, PANEL_GESTURE_SWIPE_LEFT,  NULL },
        { 367, 215,  40, 239, 12, PANEL_GESTURE_SWIPE_LEFT,  NULL },
        { 367, 270,  78, 216,  4, PANEL_GESTURE_SWIPE_LEFT,  NULL },
        /* --- short drags: past the tap radius, short of the swipe --- */
        {   1, 285,  26, 281,  2, PANEL_GESTURE_NONE, NULL },
        {   2, 279,  44, 290,  7, PANEL_GESTURE_NONE, NULL },
        /* --- and one that stayed inside the tap radius --- */
        {   1, 326,  11, 329,  2, PANEL_GESTURE_TAP, NULL },
        /* 143 px across in 2 samples, inside an interior: a swipe either way */
        {   1, 281, 144, 277,  2, PANEL_GESTURE_SWIPE_RIGHT, NULL },
    };

    unsigned taps_from_one_sample = 0, rights = 0;
    for (unsigned i = 0; i < sizeof k / sizeof k[0]; i++) {
        panel_press_t p = press(k[i].x0, k[i].y0, k[i].x1, k[i].y1, k[i].n);
        const panel_gesture_t g = panel_gesture_classify(&p);
        CHECK(g == k[i].want, "captured press %u (%d,%d)->(%d,%d) n=%u%s%s: "
              "wanted %s, got %s", i, k[i].x0, k[i].y0, k[i].x1, k[i].y1, k[i].n,
              k[i].panel_said ? " -- panel said " : "",
              k[i].panel_said ? k[i].panel_said : "",
              name(k[i].want), name(g));
        if (k[i].n == 1 && g == PANEL_GESTURE_TAP) taps_from_one_sample++;
        if (g == PANEL_GESTURE_SWIPE_RIGHT) rights++;
    }

    /* NOT ONE of the eight single-sample presses may be a tap. The old code
     * made all eight taps, and three of them did something visible the
     * operator had not asked for. */
    CHECK(taps_from_one_sample == 0,
          "%u single-sample presses still fire taps", taps_from_one_sample);

    /* AND RIGHT SWIPES STILL WORK. Three registered on hardware before this
     * change. An early write-up claimed none ever had -- that came from a
     * parser blind to x <= 99, which is where a rightward swipe begins, and
     * the claim went into a results doc as a finding. The count is pinned here
     * so the false version cannot come back quietly. */
    CHECK(rights == 4, "expected 4 rightward verdicts, got %u", rights);
}

int main(void)
{
    test_a_press_seen_once_is_never_a_tap();
    test_a_voided_press_means_nothing();
    test_the_swipe_thresholds();
    test_the_thresholds_are_the_values_we_think();
    test_a_swipe_must_be_horizontal();
    test_a_drag_out_and_back_is_not_a_tap();
    test_the_captured_gestures_classify_as_measured();

    if (failures == 0) printf("PASS: %u checks, 0 failures\n", checks);
    else               printf("FAIL: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
