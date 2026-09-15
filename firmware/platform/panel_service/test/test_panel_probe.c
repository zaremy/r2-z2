/* Host tests for the hardware test's verdict (#101 child 6).
 *
 * The illegal cases first, and here they are all one shape: a test that
 * reports success it did not earn. A probe that asked nothing, a probe whose
 * link died, a probe counting replies that belong to somebody else.
 */
#include <stdio.h>
#include <string.h>

#include "panel_probe.h"

static int failures = 0, checks = 0;
#define CHECK(cond, ...) do {                                \
    checks++;                                                \
    if (!(cond)) { failures++;                               \
        printf("  FAIL: "); printf(__VA_ARGS__);             \
        printf("\n        at %s:%d\n", __FILE__, __LINE__); }\
} while (0)

static const char *word(const panel_probe_t *p)
{
    static char buf[16];
    return panel_probe_word(p, buf, sizeof buf);
}

/* ---- a test that asked nothing cannot pass ------------------------------- */

static void test_asking_nothing_is_not_running(void)
{
    /* The gate refused every op, or the link went before they left. The rung
     * must not sit there looking busy, and must never settle as a pass. */
    panel_probe_t p; panel_probe_init(&p);
    panel_probe_start(&p, 1000, 0, true);
    CHECK(p.state == PANEL_PROBE_NO_REPLY, "expected NO REPLY, got %d", (int)p.state);
    CHECK(strcmp(word(&p), "NO REPLY") == 0, "reads '%s'", word(&p));
    CHECK(!panel_probe_passed(&p), "a probe that asked nothing passed");
    CHECK(panel_probe_settled(&p), "it should be settled, not waiting");

    /* And no number of replies afterwards turns it into a pass: they belong
     * to something else, because this test never asked. */
    for (uint32_t t = 1000; t < 9000; t += 500)
        panel_probe_step(&p, t, 3, true);
    CHECK(p.state == PANEL_PROBE_NO_REPLY, "later replies rescued it: %d", (int)p.state);
}

static void test_a_dead_link_is_not_his_silence(void)
{
    panel_probe_t p; panel_probe_init(&p);
    panel_probe_start(&p, 1000, 3, true);
    CHECK(panel_probe_step(&p, 1100, 0, false) == PANEL_PROBE_LINK_LOST,
          "a dropped link read as %d", (int)p.state);
    CHECK(strcmp(word(&p), "LINK LOST") == 0, "reads '%s'", word(&p));
    CHECK(!panel_probe_passed(&p), "a lost link passed");
    /* It sticks: a link that returns does not retroactively pass the test. */
    panel_probe_step(&p, 1200, 3, true);
    CHECK(p.state == PANEL_PROBE_LINK_LOST, "the verdict changed after settling");
}

static void test_answers_that_arrive_late_do_not_pass(void)
{
    panel_probe_t p; panel_probe_init(&p);
    panel_probe_start(&p, 1000, 3, true);
    CHECK(panel_probe_step(&p, 1000 + PANEL_PROBE_TIMEOUT_MS - 1, 0, true)
              == PANEL_PROBE_RUNNING, "gave up early");
    CHECK(panel_probe_step(&p, 1000 + PANEL_PROBE_TIMEOUT_MS, 0, true)
              == PANEL_PROBE_NO_REPLY, "did not give up at the timeout");
    panel_probe_step(&p, 5000, 3, true);
    CHECK(p.state == PANEL_PROBE_NO_REPLY, "late replies turned it into %d", (int)p.state);
}

/* ---- and the honest outcomes -------------------------------------------- */

static void test_every_answer_is_a_pass(void)
{
    panel_probe_t p; panel_probe_init(&p);
    CHECK(strcmp(word(&p), "RUN") == 0, "idle reads '%s'", word(&p));
    panel_probe_start(&p, 1000, 3, true);
    CHECK(strcmp(word(&p), "...") == 0, "running reads '%s'", word(&p));
    CHECK(panel_probe_step(&p, 1100, 1, true) == PANEL_PROBE_RUNNING, "one reply ended it");
    CHECK(panel_probe_step(&p, 1200, 3, true) == PANEL_PROBE_PASS, "three replies did not pass");
    CHECK(strcmp(word(&p), "3/3 OK") == 0, "reads '%s'", word(&p));
    CHECK(panel_probe_passed(&p) && panel_probe_settled(&p), "pass is not settled");
}

static void test_some_answers_is_partial(void)
{
    panel_probe_t p; panel_probe_init(&p);
    panel_probe_start(&p, 1000, 3, true);
    panel_probe_step(&p, 1100, 2, true);
    CHECK(panel_probe_step(&p, 1000 + PANEL_PROBE_TIMEOUT_MS, 2, true)
              == PANEL_PROBE_PARTIAL, "two of three was not partial");
    CHECK(strcmp(word(&p), "2/3") == 0, "reads '%s'", word(&p));
    CHECK(!panel_probe_passed(&p), "a partial result passed");
}

static void test_the_clock_wrapping_does_not_break_it(void)
{
    /* now_ms wraps every 49.7 days, and a test started just before it must
     * still time out on time rather than never. */
    panel_probe_t p; panel_probe_init(&p);
    const uint32_t t0 = 0xFFFFF000u;          /* 4096 ms before the wrap */
    panel_probe_start(&p, t0, 3, true);
    CHECK(panel_probe_step(&p, t0 + 100u, 0, true) == PANEL_PROBE_RUNNING,
          "gave up before the wrap");
    CHECK(panel_probe_step(&p, t0 + PANEL_PROBE_TIMEOUT_MS, 0, true)
              == PANEL_PROBE_NO_REPLY, "never timed out across the wrap: %d", (int)p.state);
}

static void test_more_answers_than_asked_still_passes(void)
{
    /* Readings can arrive that this test did not ask for -- the periodic
     * battery poll lands in the same three stamps. More than asked is a pass,
     * not a fault; what matters is that FEWER never is. */
    panel_probe_t p; panel_probe_init(&p);
    panel_probe_start(&p, 1000, 3, true);
    CHECK(panel_probe_step(&p, 1100, 9, true) == PANEL_PROBE_PASS, "extra replies failed");
}

static void test_nulls_and_a_short_buffer(void)
{
    char buf[4] = { 'x', 'x', 'x', 'x' };
    panel_probe_init(NULL);
    CHECK(panel_probe_step(NULL, 1, 1, true) == PANEL_PROBE_IDLE, "null stepped");
    CHECK(!panel_probe_passed(NULL) && !panel_probe_settled(NULL), "null passed");
    CHECK(panel_probe_word(NULL, buf, sizeof buf)[0] == '\0', "null wrote a word");
    panel_probe_t p; panel_probe_init(&p);
    panel_probe_start(&p, 0, 3, true);
    panel_probe_step(&p, 9999, 0, true);
    panel_probe_word(&p, buf, 4);
    CHECK(buf[3] == '\0', "a short buffer was overrun");
    CHECK(panel_probe_word(&p, NULL, 8)[0] == '\0', "a null buffer returned text");
}

/* ---- may a tap start a test? ------------------------------------------- */

static void test_a_test_already_under_way_refuses_the_tap(void)
{
    /* THE ILLEGAL CASES FIRST, and there are four of them because the test
     * passes through four states before it is over. Each one was a real
     * window at some point in this feature's review: a second tap inside it
     * bought three more ops for one intended test. */
    CHECK(!panel_probe_may_start((panel_probe_gate_t){.queued = true}),
          "a queued request did not refuse the tap");
    CHECK(!panel_probe_may_start((panel_probe_gate_t){.in_flight = true}),
          "ops in flight did not refuse the tap");
    CHECK(!panel_probe_may_start((panel_probe_gate_t){.pending = true}),
          "a sent-but-unclocked test did not refuse the tap");
    CHECK(!panel_probe_may_start((panel_probe_gate_t){.state = PANEL_PROBE_RUNNING}),
          "a running test did not refuse the tap");

    /* And any combination of them, since they overlap in practice. */
    CHECK(!panel_probe_may_start((panel_probe_gate_t){
              .queued = true, .in_flight = true, .pending = true,
              .state = PANEL_PROBE_RUNNING}),
          "all four at once did not refuse the tap");
}

static void test_a_settled_test_lets_the_next_tap_through(void)
{
    /* Nothing under way: the tap is admitted. A panel that refused forever
     * after one test would be worse than one that never ran a test, because
     * it would look identical to a working one. */
    CHECK(panel_probe_may_start((panel_probe_gate_t){0}),
          "a fresh panel refused the first tap");

    /* EVERY settled verdict must let the operator try again -- including the
     * failures, which are exactly when they will want to. */
    const panel_probe_state_t settled[] = {
        PANEL_PROBE_IDLE, PANEL_PROBE_PASS, PANEL_PROBE_PARTIAL,
        PANEL_PROBE_NO_REPLY, PANEL_PROBE_LINK_LOST,
    };
    for (unsigned i = 0; i < sizeof settled / sizeof settled[0]; i++)
        CHECK(panel_probe_may_start((panel_probe_gate_t){.state = settled[i]}),
              "a settled test (%u) refused the next tap", (unsigned)settled[i]);
}

static void test_a_pass_is_not_mistaken_for_a_test_in_progress(void)
{
    /* The stale-green bug from the guard's side: after a PASS the panel must
     * admit the next tap, and it is the RENDERER's job to stop showing the old
     * verdict. If this ever starts refusing, the operator gets a rung that
     * looks tappable, reads a cheerful green, and does nothing when pressed --
     * indistinguishable from a working panel. */
    CHECK(panel_probe_may_start((panel_probe_gate_t){.state = PANEL_PROBE_PASS}),
          "a settled PASS refused the next tap");
    CHECK(!panel_probe_may_start((panel_probe_gate_t){
              .queued = true, .state = PANEL_PROBE_PASS}),
          "a PASS with a tap already queued admitted another");
}

static void test_the_guard_covers_the_whole_life_of_a_test(void)
{
    /* WALK ONE TEST THROUGH, in the order the two tasks actually produce.
     * The point of this one is that there is no seam between the steps: a tap
     * is refused continuously from the moment it is accepted until the verdict
     * settles. Checking the states one at a time cannot show that. */
    panel_probe_gate_t g = {0};

    CHECK(panel_probe_may_start(g), "step 0");

    g.queued = true;                                 /* the tap lands */
    CHECK(!panel_probe_may_start(g), "step 1");

    g.queued = false; g.in_flight = true;            /* the link task takes it */
    CHECK(!panel_probe_may_start(g), "step 2");

    g.in_flight = false; g.pending = true;           /* the ops are away */
    CHECK(!panel_probe_may_start(g), "step 3");

    g.pending = false; g.state = PANEL_PROBE_RUNNING;/* the clock starts */
    CHECK(!panel_probe_may_start(g), "step 4");

    g.state = PANEL_PROBE_PASS;                      /* and it settles */
    CHECK(panel_probe_may_start(g), "step 5");
}

/* ---- a tap made while he is away ---------------------------------------- */

static void test_a_test_sent_with_the_link_down_blames_the_link(void)
{
    /* THE ILLEGAL ANSWER FIRST: NO REPLY. With him away every send fails, so
     * nothing went out -- which at this layer looks exactly like a gate that
     * refused every op, and the two deserve opposite answers. Calling it NO
     * REPLY blames him for a silence that is entirely ours, and it is the
     * COMMON case: the rare one is a link that drops mid-window, and only
     * that one was ever handled. */
    panel_probe_t p;
    panel_probe_init(&p);
    panel_probe_start(&p, 1000, 0, false);
    CHECK(p.state == PANEL_PROBE_LINK_LOST,
          "a test sent with the link down did not read LINK LOST (%d)", (int)p.state);
    CHECK(panel_probe_settled(&p), "LINK LOST did not settle");
    CHECK(!panel_probe_passed(&p), "LINK LOST passed");

    /* Even if ops somehow reported away, the link was down when they went:
     * the verdict is about the link, not the count. */
    panel_probe_init(&p);
    panel_probe_start(&p, 1000, 3, false);
    CHECK(p.state == PANEL_PROBE_LINK_LOST,
          "a down link with ops away did not read LINK LOST");

    /* And a LIVE link that sent nothing is still NO REPLY. That is the gate
     * refusing, which is also our doing and not his silence -- but it is not
     * the link's doing either, and must not say so. */
    panel_probe_init(&p);
    panel_probe_start(&p, 1000, 0, true);
    CHECK(p.state == PANEL_PROBE_NO_REPLY,
          "a live link that sent nothing did not read NO REPLY");
    CHECK(!panel_probe_passed(&p), "NO REPLY passed");
}

/* ---- #168 part 4: a PASS that means R2 answered THESE questions ---------- */

static void test_a_poll_reply_cannot_answer_a_question_nobody_asked(void)
{
    /* THE ILLEGAL CASE, and the whole reason the ledger exists. The panel
     * polls battery every 15 s and dome every 30 s on their own seqs. Under
     * the old count-what-arrived rule those replies counted toward a running
     * test; with the sequence gate (D-025) keying on a PASS, that meant STANCE
     * could open on a battery poll with the dome never having moved. */
    panel_ledger_t l;
    panel_ledger_reset(&l);
    CHECK(panel_ledger_add(&l, 10), "seq 10 refused");
    CHECK(panel_ledger_add(&l, 11), "seq 11 refused");

    CHECK(!panel_ledger_note(&l, 200), "a poll on seq 200 answered our test");
    CHECK(!panel_ledger_note(&l, 12),  "an unasked seq answered our test");
    CHECK(panel_ledger_answered(&l) == 0,
          "%u answers from replies we never asked for", panel_ledger_answered(&l));

    CHECK(panel_ledger_note(&l, 11), "our own reply on seq 11 did not count");
    CHECK(panel_ledger_answered(&l) == 1, "our own reply was not counted");
}

static void test_a_duplicate_reply_is_not_a_second_answer(void)
{
    /* R2 can repeat, and a retry can echo. Striking a seq twice would let a
     * three-question test pass on two answers and one duplicate. */
    panel_ledger_t l;
    panel_ledger_reset(&l);
    panel_ledger_add(&l, 7);
    panel_ledger_add(&l, 8);
    panel_ledger_add(&l, 9);

    CHECK(panel_ledger_note(&l, 8), "first reply on 8 did not count");
    CHECK(!panel_ledger_note(&l, 8), "a duplicate reply on 8 counted again");
    CHECK(!panel_ledger_note(&l, 8), "and again");
    CHECK(panel_ledger_answered(&l) == 1,
          "%u answers from one reply", panel_ledger_answered(&l));
    CHECK(panel_ledger_asked(&l) == 3, "asked count drifted");
}

static void test_a_seq_already_outstanding_is_refused(void)
{
    /* Two open slots on one seq means one reply strikes both. The ledger must
     * refuse to get into that state rather than mis-count its way out. */
    panel_ledger_t l;
    panel_ledger_reset(&l);
    CHECK(panel_ledger_add(&l, 5), "first add refused");
    CHECK(!panel_ledger_add(&l, 5), "a duplicate outstanding seq was accepted");
    CHECK(panel_ledger_asked(&l) == 1, "the refused add still counted");

    /* Once struck, the seq is free again -- it is no longer outstanding. */
    CHECK(panel_ledger_note(&l, 5), "reply did not strike");
    CHECK(panel_ledger_add(&l, 5), "a closed seq could not be reused");
}

static void test_the_ledger_refuses_to_overflow(void)
{
    panel_ledger_t l;
    panel_ledger_reset(&l);
    for (unsigned i = 0; i < PANEL_LEDGER_MAX; i++)
        CHECK(panel_ledger_add(&l, (uint8_t)i), "add %u refused early", i);
    CHECK(!panel_ledger_add(&l, 99), "the ledger accepted a ninth request");
    CHECK(panel_ledger_asked(&l) == PANEL_LEDGER_MAX, "asked count overflowed");
}

static void test_a_null_ledger_answers_nothing(void)
{
    CHECK(!panel_ledger_add(NULL, 1), "NULL ledger accepted a request");
    CHECK(!panel_ledger_note(NULL, 1), "NULL ledger struck a request");
    CHECK(panel_ledger_asked(NULL) == 0, "NULL ledger asked something");
    CHECK(panel_ledger_answered(NULL) == 0, "NULL ledger answered something");
    panel_ledger_reset(NULL);       /* must not crash */
}

static void test_only_measured_tiers_have_a_timeout(void)
{
    /* THE GUARD IS THE ZERO. READ and DOME have numbers because someone
     * measured them; every other tier returns 0 so the caller refuses to run
     * it. A guessed constant would read exactly like a measured one later. */
    CHECK(panel_probe_timeout_ms(0) == PANEL_PROBE_TIMEOUT_MS,
          "READ lost its measured 2 s");
    CHECK(panel_probe_timeout_ms(3) == PANEL_PROBE_DOME_TIMEOUT_MS,
          "DOME lost its D-013 window");

    /* D-013 measured 2.0-2.2 s REGARDLESS of distance. A window that does not
     * clear the move calls a working move PARTIAL -- the bug this replaces. */
    CHECK(panel_probe_timeout_ms(3) > 2200u,
          "the DOME window (%u ms) does not clear a 2.2 s move",
          (unsigned)panel_probe_timeout_ms(3));

    const int unmeasured[] = { 1, 2, 4 };          /* LEDS, AUDIO, STANCE */
    for (unsigned i = 0; i < sizeof unmeasured / sizeof unmeasured[0]; i++)
        CHECK(panel_probe_timeout_ms(unmeasured[i]) == 0u,
              "tier %d has a timeout nobody measured", unmeasured[i]);

    const int bad[] = { -1, 5, 6, 99, -2147483647 - 1 };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++)
        CHECK(panel_probe_timeout_ms(bad[i]) == 0u,
              "out-of-range tier %d got a timeout", bad[i]);
}

int main(void)
{
    test_a_poll_reply_cannot_answer_a_question_nobody_asked();
    test_a_duplicate_reply_is_not_a_second_answer();
    test_a_seq_already_outstanding_is_refused();
    test_the_ledger_refuses_to_overflow();
    test_a_null_ledger_answers_nothing();
    test_only_measured_tiers_have_a_timeout();

    test_a_test_sent_with_the_link_down_blames_the_link();
    test_a_test_already_under_way_refuses_the_tap();
    test_a_settled_test_lets_the_next_tap_through();
    test_a_pass_is_not_mistaken_for_a_test_in_progress();
    test_the_guard_covers_the_whole_life_of_a_test();
    test_asking_nothing_is_not_running();
    test_a_dead_link_is_not_his_silence();
    test_answers_that_arrive_late_do_not_pass();
    test_every_answer_is_a_pass();
    test_some_answers_is_partial();
    test_the_clock_wrapping_does_not_break_it();
    test_more_answers_than_asked_still_passes();
    test_nulls_and_a_short_buffer();
    printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
