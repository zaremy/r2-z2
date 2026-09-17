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
    panel_probe_start(&p, 1000, 0, true, 0);
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
    panel_probe_start(&p, 1000, 3, true, 0);
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
    panel_probe_start(&p, 1000, 3, true, 0);
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
    panel_probe_start(&p, 1000, 3, true, 0);
    CHECK(strcmp(word(&p), "...") == 0, "running reads '%s'", word(&p));
    CHECK(panel_probe_step(&p, 1100, 1, true) == PANEL_PROBE_RUNNING, "one reply ended it");
    CHECK(panel_probe_step(&p, 1200, 3, true) == PANEL_PROBE_PASS, "three replies did not pass");
    CHECK(strcmp(word(&p), "3/3 OK") == 0, "reads '%s'", word(&p));
    CHECK(panel_probe_passed(&p) && panel_probe_settled(&p), "pass is not settled");
}

static void test_some_answers_is_partial(void)
{
    panel_probe_t p; panel_probe_init(&p);
    panel_probe_start(&p, 1000, 3, true, 0);
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
    panel_probe_start(&p, t0, 3, true, 0);
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
    panel_probe_start(&p, 1000, 3, true, 0);
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
    panel_probe_start(&p, 0, 3, true, 0);
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
    CHECK(panel_probe_may_start((panel_probe_gate_t){.motion_settled = true}),
          "a fresh panel refused the first tap");

    /* EVERY settled verdict must let the operator try again -- including the
     * failures, which are exactly when they will want to. */
    const panel_probe_state_t settled[] = {
        PANEL_PROBE_IDLE, PANEL_PROBE_PASS, PANEL_PROBE_PARTIAL,
        PANEL_PROBE_NO_REPLY, PANEL_PROBE_LINK_LOST,
    };
    for (unsigned i = 0; i < sizeof settled / sizeof settled[0]; i++)
        CHECK(panel_probe_may_start((panel_probe_gate_t){
                  .state = settled[i], .motion_settled = true}),
              "a settled test (%u) refused the next tap", (unsigned)settled[i]);
}

static void test_a_pass_is_not_mistaken_for_a_test_in_progress(void)
{
    /* The stale-green bug from the guard's side: after a PASS the panel must
     * admit the next tap, and it is the RENDERER's job to stop showing the old
     * verdict. If this ever starts refusing, the operator gets a rung that
     * looks tappable, reads a cheerful green, and does nothing when pressed --
     * indistinguishable from a working panel. */
    CHECK(panel_probe_may_start((panel_probe_gate_t){
              .state = PANEL_PROBE_PASS, .motion_settled = true}),
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
    panel_probe_gate_t g = {.motion_settled = true};

    CHECK(panel_probe_may_start(g), "step 0");

    g.queued = true;                                 /* the tap lands */
    CHECK(!panel_probe_may_start(g), "step 1");

    g.queued = false; g.in_flight = true;            /* the link task takes it */
    CHECK(!panel_probe_may_start(g), "step 2");

    g.in_flight = false; g.pending = true;           /* the ops are away */
    CHECK(!panel_probe_may_start(g), "step 3");

    g.pending = false; g.state = PANEL_PROBE_RUNNING;/* the clock starts */
    g.motion_settled = false;                        /* and so does he */
    CHECK(!panel_probe_may_start(g), "step 4");

    g.state = PANEL_PROBE_PASS;                      /* and it settles */
    CHECK(!panel_probe_may_start(g),
          "step 5 -- the verdict settled but he is still moving");

    g.motion_settled = true;                         /* and he stops */
    CHECK(panel_probe_may_start(g), "step 6");
}

/* ---- #184: the guard has to outlast the move, not the verdict ------------ */

static void test_the_guard_holds_while_he_is_still_moving(void)
{
    /* THE ILLEGAL CASE FIRST. Everything about the TEST says done -- nothing
     * queued, nothing in flight, nothing pending, a settled PASS on the glass
     * -- and he is still travelling. This is the exact state the old guard
     * admitted a second tap in, and a second dome move fired into the first. */
    CHECK(!panel_probe_may_start((panel_probe_gate_t){
              .state = PANEL_PROBE_PASS, .motion_settled = false}),
          "a settled PASS admitted a tap while he was still moving");

    /* AND OMITTING THE FIELD MUST REFUSE, not admit. C zero-fills a
     * designated initializer, so the caller that forgets is the caller this
     * has to be safe for -- the tap handler lives in an LVGL file with no
     * host harness, where a forgotten field compiles silently. Phrasing the
     * field as the SAFE condition is what makes the zero the refusal. */
    CHECK(!panel_probe_may_start((panel_probe_gate_t){.state = PANEL_PROBE_PASS}),
          "omitting motion_settled admitted the tap -- the default is unsafe");
}

static void test_motion_outlasts_the_verdict_on_a_moving_tier(void)
{
    /* THE WHOLE DEFECT IN ONE ASSERTION. READ's window is 2000 ms and D-013
     * measured the longest dome move at 2190 ms, so between those two numbers
     * the old guard was open and he was moving. */
    CHECK(PANEL_PROBE_DOME_MOVE_MS > PANEL_PROBE_TIMEOUT_MS,
          "the move no longer outlasts the read window -- this test is moot");

    panel_probe_t dome;
    panel_probe_init(&dome);
    panel_probe_arm_completion(&dome, true);
    panel_probe_start(&dome, 1000, 1, true, 3);

    /* Answered immediately and completed: the VERDICT is done at once. */
    panel_probe_note_complete(&dome);
    CHECK(panel_probe_step(&dome, 1010, 1, true) == PANEL_PROBE_PASS,
          "an answered, completed dome test did not pass");
    CHECK(panel_probe_settled(&dome), "a passed test is not settled");

    /* And he is STILL MOVING, by his own measured duration. */
    CHECK(!panel_probe_motion_settled(&dome, 1000 + PANEL_PROBE_DOME_MOVE_MS - 1),
          "he was called stopped one ms before the measured move ends");
    CHECK(panel_probe_busy(&dome, 1000 + PANEL_PROBE_TIMEOUT_MS),
          "busy went false at the READ window while the move was still running");
    CHECK(panel_probe_busy(&dome, 1000 + PANEL_PROBE_DOME_MOVE_MS - 1),
          "busy went false one ms before the measured move ends");

    /* Only then does it release. */
    CHECK(panel_probe_motion_settled(&dome, 1000 + PANEL_PROBE_DOME_MOVE_MS),
          "he was still called moving after the measured move ended");
    CHECK(!panel_probe_busy(&dome, 1000 + PANEL_PROBE_DOME_MOVE_MS),
          "busy stayed true after both the verdict and the move were done");

    /* A READ test has no move to outlast: busy tracks the verdict alone. */
    panel_probe_t read;
    panel_probe_init(&read);
    panel_probe_start(&read, 1000, 1, true, 0);
    CHECK(panel_probe_busy(&read, 1001), "a running READ test was not busy");
    CHECK(panel_probe_step(&read, 1010, 1, true) == PANEL_PROBE_PASS,
          "READ did not pass on its answers alone");
    CHECK(!panel_probe_busy(&read, 1011),
          "a settled READ test stayed busy -- reading cannot move him");
}

static void test_a_moving_tier_needs_a_completion_not_a_reply_count(void)
{
    /* THE ILLEGAL CASE: every answer is in, and he never said he finished.
     * A reply says the op was heard. On a tier that moves him that is not the
     * same question as whether the move happened, and answering it as though
     * it were is how a PASS gets awarded to a droid that never turned. */
    panel_probe_t p;
    panel_probe_init(&p);
    panel_probe_arm_completion(&p, true);
    panel_probe_start(&p, 1000, 2, true, 3);
    CHECK(panel_probe_step(&p, 1010, 2, true) == PANEL_PROBE_RUNNING,
          "a dome test passed on replies alone, with no completion");
    CHECK(!panel_probe_passed(&p), "it reported passed with no completion");

    /* The completion arrives and it passes. */
    panel_probe_note_complete(&p);
    CHECK(panel_probe_step(&p, 1020, 2, true) == PANEL_PROBE_PASS,
          "a dome test with answers AND a completion did not pass");

    /* And without one it times out rather than hanging -- PARTIAL, because
     * the answers did arrive; the move is what did not report. */
    panel_probe_t q;
    panel_probe_init(&q);
    panel_probe_arm_completion(&q, true);
    panel_probe_start(&q, 1000, 2, true, 3);
    CHECK(panel_probe_step(&q, 1000 + PANEL_PROBE_DOME_TIMEOUT_MS, 2, true)
              == PANEL_PROBE_PARTIAL,
          "a dome test with no completion never gave up");

    /* READ needs no completion -- it cannot move him, so there is nothing to
     * complete, and requiring one would deadlock the only runnable tier. */
    CHECK(!panel_probe_needs_completion(0), "READ was made to need a completion");
    CHECK(panel_probe_needs_completion(3), "DOME does not need a completion");
    CHECK(panel_probe_needs_completion(4), "STANCE does not need a completion");
}

static void test_an_unarmed_completion_channel_is_refused(void)
{
    /* PROVE THE CHANNEL LIVE BEFORE TRUSTING ITS SILENCE. leg_action_complete
     * does not fire unless the notify went out, and a session that forgets
     * still gets animation_complete and still measures durations -- it just
     * sees zero leg events, which is a wrong answer in the reassuring
     * direction. So a moving tier whose channel was never armed is REFUSED,
     * the same way an unmeasured window is, rather than run and blamed on
     * him. */
    panel_probe_t p;
    panel_probe_init(&p);
    /* no panel_probe_arm_completion -- the default */
    panel_probe_start(&p, 1000, 3, true, 3);
    CHECK(p.state == PANEL_PROBE_NO_REPLY,
          "an unarmed dome test ran anyway");
    CHECK(!panel_probe_passed(&p), "an unarmed dome test passed");

    /* Armed, it runs. */
    panel_probe_t q;
    panel_probe_init(&q);
    panel_probe_arm_completion(&q, true);
    panel_probe_start(&q, 1000, 3, true, 3);
    CHECK(q.state == PANEL_PROBE_RUNNING, "an armed dome test did not run");

    /* Arming is not sticky across init: one armed run must not vouch for the
     * next. */
    panel_probe_init(&q);
    panel_probe_start(&q, 2000, 3, true, 3);
    CHECK(q.state == PANEL_PROBE_NO_REPLY,
          "arming survived init and vouched for the next run");

    /* Nor is a completion itself sticky across runs. */
    panel_probe_t r;
    panel_probe_init(&r);
    panel_probe_arm_completion(&r, true);
    panel_probe_start(&r, 1000, 1, true, 3);
    panel_probe_note_complete(&r);
    CHECK(panel_probe_step(&r, 1010, 1, true) == PANEL_PROBE_PASS, "first run");
    panel_probe_start(&r, 5000, 1, true, 3);       /* second run, same probe */
    CHECK(panel_probe_step(&r, 5010, 1, true) == PANEL_PROBE_RUNNING,
          "the previous run's completion passed the next one");

    /* READ is unaffected: it needs no completion, so it never needs arming. */
    panel_probe_t read;
    panel_probe_init(&read);
    panel_probe_start(&read, 1000, 3, true, 0);
    CHECK(read.state == PANEL_PROBE_RUNNING,
          "READ was refused for an unarmed completion channel it does not use");
}

static void test_no_tier_can_move_him_without_a_measured_duration(void)
{
    /* THE PAIRING THAT KEEPS A 0 HONEST. panel_probe_move_ms returns 0 for
     * two different reasons -- "cannot move him" and "nobody timed it" -- and
     * only the first is safe. What separates them is that every tier with an
     * unmeasured move is ALSO refused by a 0 window. Assert that directly, so
     * a future tier given a window but no move duration fails here instead of
     * releasing the guard mid-travel. */
    for (int tier = 0; tier <= 5; tier++) {
        if (panel_probe_timeout_ms(tier) == 0u) continue;   /* refused upstream */
        if (!panel_probe_needs_completion(tier)) continue;  /* cannot move him */
        CHECK(panel_probe_move_ms(tier) > 0u,
              "tier %d is runnable and moves him but has no measured move "
              "duration", tier);
    }
    CHECK(panel_probe_move_ms(0) == 0u, "READ was given a move duration");
    CHECK(panel_probe_move_ms(3) == PANEL_PROBE_DOME_MOVE_MS,
          "DOME's move duration is not the measured one");
    CHECK(PANEL_PROBE_DOME_MOVE_MS >= 2200u,
          "the move window dropped below D-013's longest measured move");
    CHECK(PANEL_PROBE_DOME_TIMEOUT_MS > PANEL_PROBE_DOME_MOVE_MS,
          "the reply window no longer clears the move it has to contain");
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
    panel_probe_start(&p, 1000, 0, false, 0);
    CHECK(p.state == PANEL_PROBE_LINK_LOST,
          "a test sent with the link down did not read LINK LOST (%d)", (int)p.state);
    CHECK(panel_probe_settled(&p), "LINK LOST did not settle");
    CHECK(!panel_probe_passed(&p), "LINK LOST passed");

    /* Even if ops somehow reported away, the link was down when they went:
     * the verdict is about the link, not the count. */
    panel_probe_init(&p);
    panel_probe_start(&p, 1000, 3, false, 0);
    CHECK(p.state == PANEL_PROBE_LINK_LOST,
          "a down link with ops away did not read LINK LOST");

    /* And a LIVE link that sent nothing is still NO REPLY. That is the gate
     * refusing, which is also our doing and not his silence -- but it is not
     * the link's doing either, and must not say so. */
    panel_probe_init(&p);
    panel_probe_start(&p, 1000, 0, true, 0);
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

    /* LITERAL MILLISECONDS, because the line above pins the function to the
     * macro and leaves the MACRO free: a mutation battery set the DOME window
     * to 2201 ms -- one millisecond for a reply after a 2.2 s move -- and the
     * whole suite stayed green. These are the numbers a dome actually meets.
     *
     * D-013 measured 2.0-2.2 s REGARDLESS of distance, so the window must
     * clear the move AND leave room for the answer. A second of slack is the
     * floor; anything tighter is the old bug with a bigger number. */
    CHECK(panel_probe_timeout_ms(3) >= 3200u,
          "the DOME window (%u ms) leaves under a second for the reply after "
          "D-013's 2.2 s move", (unsigned)panel_probe_timeout_ms(3));
    CHECK(panel_probe_timeout_ms(3) == 4000u,
          "the DOME window is no longer 4000 ms (%u)",
          (unsigned)panel_probe_timeout_ms(3));
    CHECK(panel_probe_timeout_ms(0) == 2000u,
          "the READ window is no longer 2000 ms (%u)",
          (unsigned)panel_probe_timeout_ms(0));

    const int unmeasured[] = { 1, 2, 4 };          /* LEDS, AUDIO, STANCE */
    for (unsigned i = 0; i < sizeof unmeasured / sizeof unmeasured[0]; i++)
        CHECK(panel_probe_timeout_ms(unmeasured[i]) == 0u,
              "tier %d has a timeout nobody measured", unmeasured[i]);

    const int bad[] = { -1, 5, 6, 99, -2147483647 - 1 };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++)
        CHECK(panel_probe_timeout_ms(bad[i]) == 0u,
              "out-of-range tier %d got a timeout", bad[i]);
}

static void test_an_unmeasured_tier_is_refused_not_defaulted(void)
{
    /* THE GUARD THAT MAKES THE ZERO MEAN SOMETHING. Without a caller that
     * refuses it, panel_probe_timeout_ms is a table nobody reads -- the
     * repo's own "a layer with no caller passes every check a live one
     * passes". A tier with no measured window must not run at all. */
    for (int tier = 0; tier < 6; tier++) {
        panel_probe_t p;
        panel_probe_init(&p);
        /* Armed, so this test keeps measuring the WINDOW. An unarmed moving
         * tier is refused too, for a different reason, and that refusal has
         * its own test -- leaving it in play here would let this one pass for
         * the wrong reason on the day the window gets measured. */
        panel_probe_arm_completion(&p, true);
        panel_probe_start(&p, 1000, 3, true, tier);
        if (panel_probe_timeout_ms(tier) == 0u) {
            CHECK(p.state == PANEL_PROBE_NO_REPLY,
                  "tier %d has no measured window but started anyway (state %d)",
                  tier, (int)p.state);
            CHECK(!panel_probe_passed(&p), "an unmeasured tier passed");
        } else {
            CHECK(p.state == PANEL_PROBE_RUNNING,
                  "tier %d has a window but did not run (state %d)",
                  tier, (int)p.state);
        }
    }
}

static void test_each_tier_times_out_on_its_own_window(void)
{
    /* THE WHOLE POINT OF THE TABLE. A DOME test must still be RUNNING at the
     * moment a READ test would already have given up -- that is the 2 s
     * window calling a 2.0-2.2 s move PARTIAL, which is the bug. */
    panel_probe_t read, dome;
    panel_probe_init(&read);
    panel_probe_init(&dome);
    panel_probe_arm_completion(&dome, true);   /* a moving tier is refused unarmed */
    panel_probe_start(&read, 0, 3, true, 0);
    panel_probe_start(&dome, 0, 3, true, 3);

    /* 2.5 s in: past READ's window, and past D-013's longest measured move. */
    CHECK(panel_probe_step(&read, 2500, 0, true) == PANEL_PROBE_NO_REPLY,
          "READ did not give up at 2.5 s");
    CHECK(panel_probe_step(&dome, 2500, 0, true) == PANEL_PROBE_RUNNING,
          "DOME gave up while a measured move could still be travelling");

    /* And it does eventually give up -- a longer window, not an absent one. */
    CHECK(panel_probe_step(&dome, 60000, 0, true) == PANEL_PROBE_NO_REPLY,
          "DOME never times out at all");
}

int main(void)
{
    test_an_unmeasured_tier_is_refused_not_defaulted();
    test_each_tier_times_out_on_its_own_window();

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
    test_the_guard_holds_while_he_is_still_moving();
    test_motion_outlasts_the_verdict_on_a_moving_tier();
    test_a_moving_tier_needs_a_completion_not_a_reply_count();
    test_an_unarmed_completion_channel_is_refused();
    test_no_tier_can_move_him_without_a_measured_duration();
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
