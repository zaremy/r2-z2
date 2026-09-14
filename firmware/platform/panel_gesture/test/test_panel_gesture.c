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
    /* Every gesture from the 2026-09-14 capture, by sample count and
     * endpoints. The five that worked must still work -- a fix that cured the
     * flicks by breaking the swipes would pass every test above. */
    struct { int32_t x0, y0, x1, y1; uint32_t n; panel_gesture_t want; } k[] = {
        { 367, 230, 141, 267, 7, PANEL_GESTURE_SWIPE_LEFT },
        { 362, 291, 262, 281, 4, PANEL_GESTURE_SWIPE_LEFT },
        { 360, 250, 138, 263, 15, PANEL_GESTURE_SWIPE_LEFT },
        { 367, 215, 122, 228, 10, PANEL_GESTURE_SWIPE_LEFT },
        { 367, 270, 206, 228, 3, PANEL_GESTURE_SWIPE_LEFT },
        /* the two-sample diagonal: far enough, not straight enough */
        { 367, 341, 243, 199, 2, PANEL_GESTURE_NONE },
        /* the seven single-sample flicks that used to fire taps */
        { 136, 275, 136, 275, 1, PANEL_GESTURE_NONE },
        { 254, 275, 254, 275, 1, PANEL_GESTURE_NONE },
        { 345, 299, 345, 299, 1, PANEL_GESTURE_NONE },
        { 271, 318, 271, 318, 1, PANEL_GESTURE_NONE },
        { 144, 277, 144, 277, 1, PANEL_GESTURE_NONE },
        { 358, 280, 358, 280, 1, PANEL_GESTURE_NONE },
        { 285, 300, 285, 300, 1, PANEL_GESTURE_NONE },
    };
    unsigned taps = 0;
    for (unsigned i = 0; i < sizeof k / sizeof k[0]; i++) {
        panel_press_t p = press(k[i].x0, k[i].y0, k[i].x1, k[i].y1, k[i].n);
        const panel_gesture_t g = panel_gesture_classify(&p);
        CHECK(g == k[i].want, "captured gesture %u (%d,%d)->(%d,%d) n=%u: "
              "wanted %s, got %s", i, k[i].x0, k[i].y0, k[i].x1, k[i].y1,
              k[i].n, name(k[i].want), name(g));
        if (g == PANEL_GESTURE_TAP) taps++;
    }
    /* NOT ONE of the thirteen was a tap. The operator was swiping throughout,
     * and the old code produced seven. */
    CHECK(taps == 0, "%u of the captured swipes still fire taps", taps);
}

int main(void)
{
    test_a_press_seen_once_is_never_a_tap();
    test_a_voided_press_means_nothing();
    test_the_swipe_thresholds();
    test_a_swipe_must_be_horizontal();
    test_a_drag_out_and_back_is_not_a_tap();
    test_the_captured_gestures_classify_as_measured();

    if (failures == 0) printf("PASS: %u checks, 0 failures\n", checks);
    else               printf("FAIL: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
