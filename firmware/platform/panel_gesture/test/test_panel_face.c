/* Host tests for the face's gesture table (E2E v0 slice 3.0b).
 *
 * Every context the face can be in (2^7), every input, every zone, is
 * enumerated -- so each rule below is checked against every combination, not
 * against the handful a person thought to write. Refusals come first.
 */
#include <stdio.h>
#include <string.h>

#include "panel_face.h"

static unsigned failures = 0, checks = 0;

#define CHECK(cond, ...) do {                                \
    checks++;                                                \
    if (!(cond)) { failures++;                               \
        printf("  FAIL: "); printf(__VA_ARGS__);             \
        printf("\n        at %s:%d\n", __FILE__, __LINE__); }\
} while (0)

static panel_face_ctx_t ctx_of(unsigned bits)
{
    panel_face_ctx_t c = {
        .page       = (bits & 1u) ? PANEL_PAGE_STATUS : PANEL_PAGE_OTHER,
        .blanked    = bits & 2u,
        .wake_frame = bits & 4u,
        .awake      = bits & 8u,
        .listening  = bits & 16u,
        .exchange   = bits & 32u,
        .answering  = bits & 64u,
    };
    return c;
}
#define NCTX 128u

static const panel_zone_t ZONES[] = { PANEL_ZONE_OTHER, PANEL_ZONE_WORD, PANEL_ZONE_LOWER };
#define NZONE 3u

/* ---- ILLEGAL FIRST ------------------------------------------------------- */

static void test_zero_context_does_nothing(void)
{
    printf("     an all-false context -- asleep, released, no page -- resolves nothing\n");
    const panel_face_ctx_t zero = {0};
    for (int in = 0; in < PANEL_IN__COUNT; in++)
        for (unsigned z = 0; z < NZONE; z++)
            CHECK(panel_face_resolve(&zero, (panel_input_t)in, ZONES[z]) == PANEL_ACT_NONE,
                  "zero ctx, input %d zone %u gave %s", in, z,
                  panel_face_action_name(panel_face_resolve(&zero, (panel_input_t)in, ZONES[z])));
    CHECK(panel_face_resolve(NULL, PANEL_IN_LAND, PANEL_ZONE_LOWER) == PANEL_ACT_NONE, "NULL ctx");
    const panel_face_ctx_t live = { .page = PANEL_PAGE_STATUS, .awake = true, .answering = true };
    CHECK(panel_face_resolve(&live, (panel_input_t)PANEL_IN__COUNT, PANEL_ZONE_LOWER) == PANEL_ACT_NONE,
          "an out-of-range input must do nothing");
    CHECK(panel_face_resolve(&live, (panel_input_t)-1, PANEL_ZONE_LOWER) == PANEL_ACT_NONE,
          "a negative input must do nothing");
    CHECK(panel_face_step(NULL, &live, PANEL_IN_LAND) == PANEL_ACT_NONE, "NULL press");
    const panel_face_ctx_t junk = { .page = (panel_face_page_t)7, .awake = true, .answering = true };
    CHECK(panel_face_resolve(&junk, PANEL_IN_LAND, PANEL_ZONE_LOWER) == PANEL_ACT_NONE,
          "an out-of-range page must do nothing");
}

static void test_a_covered_face_passes_nothing_through(void)
{
    printf("     blanked or under the wake frame, a press only uncovers the face\n");
    for (unsigned b = 0; b < NCTX; b++) {
        const panel_face_ctx_t c = ctx_of(b);
        if (!c.blanked && !c.wake_frame) continue;
        for (int in = 0; in < PANEL_IN__COUNT; in++)
            for (unsigned z = 0; z < NZONE; z++) {
                const panel_face_action_t a = panel_face_resolve(&c, (panel_input_t)in, ZONES[z]);
                const panel_face_action_t want = in != PANEL_IN_LAND ? PANEL_ACT_NONE
                                               : c.blanked ? PANEL_ACT_UNBLANK : PANEL_ACT_DISMISS_WAKE;
                CHECK(a == want, "ctx %u input %d zone %u: %s, want %s", b, in, z,
                      panel_face_action_name(a), panel_face_action_name(want));
            }
    }
}

static void test_each_body_action_has_its_one_door(void)
{
    printf("     STOP, TALK and WAKE/GOODNIGHT each come from exactly one place\n");
    for (unsigned b = 0; b < NCTX; b++) {
        const panel_face_ctx_t c = ctx_of(b);
        const bool open = c.page == PANEL_PAGE_STATUS && !c.blanked && !c.wake_frame;
        for (int in = 0; in < PANEL_IN__COUNT; in++)
            for (unsigned z = 0; z < NZONE; z++) {
                const panel_face_action_t a = panel_face_resolve(&c, (panel_input_t)in, ZONES[z]);
                if (a == PANEL_ACT_STOP)
                    CHECK(open && c.answering && ZONES[z] == PANEL_ZONE_LOWER && in == PANEL_IN_LAND,
                          "STOP from ctx %u input %d zone %u", b, in, z);
                if (a == PANEL_ACT_TALK)
                    CHECK(open && c.awake && !c.listening && !c.exchange && !c.answering &&
                          ZONES[z] == PANEL_ZONE_LOWER && in == PANEL_IN_HOLD,
                          "TALK from ctx %u input %d zone %u", b, in, z);
                if (a == PANEL_ACT_POWER)
                    CHECK(open && ZONES[z] == PANEL_ZONE_WORD && in == PANEL_IN_HOLD,
                          "POWER from ctx %u input %d zone %u", b, in, z);
            }
    }
}

static void test_the_zones_never_overlap(void)
{
    printf("     every row of the panel is in one zone, and the rule's gap separates the holds\n");
    int word = 0, lower = 0, gap = 0;
    for (int y = -10; y < 460; y++) {
        const panel_zone_t z = panel_face_zone(y);
        if (z == PANEL_ZONE_WORD) word++;
        else if (z == PANEL_ZONE_LOWER) lower++;
        if (y > PANEL_FACE_WORD_BOTTOM && y < PANEL_FACE_LOWER_TOP)
            CHECK(z == PANEL_ZONE_OTHER, "y=%d, in the gap, is a hold zone", y);
        if (y > PANEL_FACE_WORD_BOTTOM && y < PANEL_FACE_LOWER_TOP) gap++;
    }
    CHECK(word == PANEL_FACE_WORD_BOTTOM - PANEL_FACE_WORD_TOP + 1, "word band is %d rows", word);
    CHECK(gap >= 5, "the gap between the holds is only %d rows", gap);
    CHECK(lower > 0 && panel_face_zone(0) == PANEL_ZONE_OTHER, "chrome at y=0 must not be a hold zone");
}

/* ---- one press, at most one action --------------------------------------- */

static void test_one_press_one_action(void)
{
    printf("     every press, in every context, through every input order, fires at most once\n");
    /* The orders a real press can take: it lands, may complete a hold, and
     * lifts as a tap or a swipe. */
    static const panel_input_t SEQ[][3] = {
        { PANEL_IN_LAND, PANEL_IN_TAP,   PANEL_IN__COUNT },
        { PANEL_IN_LAND, PANEL_IN_SWIPE, PANEL_IN__COUNT },
        { PANEL_IN_LAND, PANEL_IN_HOLD,  PANEL_IN_TAP    },
        { PANEL_IN_LAND, PANEL_IN_HOLD,  PANEL_IN_SWIPE  },
    };
    static const int Y[] = { 10, 80, 300 };
    unsigned fired_twice = 0, presses = 0;
    for (unsigned b = 0; b < NCTX; b++) {
        const panel_face_ctx_t c = ctx_of(b);
        for (unsigned s = 0; s < sizeof SEQ / sizeof SEQ[0]; s++)
            for (unsigned yi = 0; yi < 3; yi++) {
                panel_face_press_t p;
                panel_face_begin(&p, Y[yi]);
                int fired = 0;
                for (int k = 0; k < 3 && SEQ[s][k] != PANEL_IN__COUNT; k++)
                    if (panel_face_step(&p, &c, SEQ[s][k]) != PANEL_ACT_NONE) fired++;
                presses++;
                if (fired > 1) fired_twice++;
            }
    }
    CHECK(fired_twice == 0, "%u of %u presses fired more than once", fired_twice, presses);
}

static void test_the_press_is_spent_by_what_it_does_first(void)
{
    printf("     the landing that stops him cannot also become a tap or a hold\n");
    const panel_face_ctx_t c = { .page = PANEL_PAGE_STATUS, .awake = true, .exchange = true, .answering = true };
    panel_face_press_t p;
    panel_face_begin(&p, 300);
    CHECK(panel_face_step(&p, &c, PANEL_IN_LAND) == PANEL_ACT_STOP, "the landing is the STOP");
    CHECK(panel_face_step(&p, &c, PANEL_IN_HOLD) == PANEL_ACT_NONE, "and the hold after it is nothing");
    CHECK(panel_face_step(&p, &c, PANEL_IN_TAP) == PANEL_ACT_NONE, "and the lift is nothing");

    const panel_face_ctx_t blank = { .page = PANEL_PAGE_STATUS, .blanked = true, .awake = true };
    panel_face_begin(&p, 300);
    CHECK(panel_face_step(&p, &blank, PANEL_IN_LAND) == PANEL_ACT_UNBLANK, "lands: unblank");
    const panel_face_ctx_t lit = { .page = PANEL_PAGE_STATUS, .awake = true };
    CHECK(panel_face_step(&p, &lit, PANEL_IN_HOLD) == PANEL_ACT_NONE,
          "the same finger, still down on the now-lit face, must not start TALK");
}

/* ---- the rules, one by one ----------------------------------------------- */

static void test_the_spec_cases(void)
{
    printf("     spec §9's cases\n");
    const panel_face_ctx_t idle = { .page = PANEL_PAGE_STATUS, .awake = true };
    CHECK(panel_face_resolve(&idle, PANEL_IN_HOLD, PANEL_ZONE_LOWER) == PANEL_ACT_TALK, "hold lower at rest -> TALK");
    CHECK(panel_face_resolve(&idle, PANEL_IN_TAP,  PANEL_ZONE_LOWER) == PANEL_ACT_NONE, "a short press is not a question");
    CHECK(panel_face_resolve(&idle, PANEL_IN_HOLD, PANEL_ZONE_WORD)  == PANEL_ACT_POWER, "hold word -> GOODNIGHT");
    CHECK(panel_face_resolve(&idle, PANEL_IN_SWIPE, PANEL_ZONE_LOWER) == PANEL_ACT_PAGE, "swipe -> page");

    const panel_face_ctx_t released = { .page = PANEL_PAGE_STATUS };
    CHECK(panel_face_resolve(&released, PANEL_IN_HOLD, PANEL_ZONE_LOWER) == PANEL_ACT_NONE, "no TALK while released");
    CHECK(panel_face_resolve(&released, PANEL_IN_HOLD, PANEL_ZONE_WORD)  == PANEL_ACT_POWER, "the word still WAKEs him");

    const panel_face_ctx_t thinking = { .page = PANEL_PAGE_STATUS, .awake = true, .exchange = true };
    CHECK(panel_face_resolve(&thinking, PANEL_IN_HOLD, PANEL_ZONE_LOWER) == PANEL_ACT_NONE, "no second TALK while thinking");
    CHECK(panel_face_resolve(&thinking, PANEL_IN_LAND, PANEL_ZONE_LOWER) == PANEL_ACT_NONE, "no STOP while nothing can move");
    CHECK(panel_face_resolve(&thinking, PANEL_IN_HOLD, PANEL_ZONE_WORD)  == PANEL_ACT_POWER, "GOODNIGHT wins mid-exchange");

    const panel_face_ctx_t answering = { .page = PANEL_PAGE_STATUS, .awake = true, .exchange = true, .answering = true };
    CHECK(panel_face_resolve(&answering, PANEL_IN_LAND, PANEL_ZONE_LOWER) == PANEL_ACT_STOP, "STOP on the landing");
    CHECK(panel_face_resolve(&answering, PANEL_IN_HOLD, PANEL_ZONE_WORD)  == PANEL_ACT_POWER, "GOODNIGHT while answering");

    const panel_face_ctx_t listening = { .page = PANEL_PAGE_STATUS, .awake = true, .listening = true };
    CHECK(panel_face_resolve(&listening, PANEL_IN_SWIPE, PANEL_ZONE_LOWER) == PANEL_ACT_NONE, "no page flip under a held TALK");

    const panel_face_ctx_t service = { .page = PANEL_PAGE_OTHER, .awake = true, .answering = true };
    CHECK(panel_face_resolve(&service, PANEL_IN_TAP, PANEL_ZONE_LOWER) == PANEL_ACT_ROW, "SERVICE taps are rows");
    CHECK(panel_face_resolve(&service, PANEL_IN_LAND, PANEL_ZONE_LOWER) == PANEL_ACT_NONE,
          "the face STOP is a FACE control, not reachable from SERVICE");
}

int main(void)
{
    printf("panel_face host tests\n");
    test_zero_context_does_nothing();
    test_a_covered_face_passes_nothing_through();
    test_each_body_action_has_its_one_door();
    test_the_zones_never_overlap();
    test_one_press_one_action();
    test_the_press_is_spent_by_what_it_does_first();
    test_the_spec_cases();
    if (failures == 0) printf("PASS: %u checks, 0 failures\n", checks);
    else               printf("FAIL: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
