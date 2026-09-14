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
    panel_probe_start(&p, 1000, 0);
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
    panel_probe_start(&p, 1000, 3);
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
    panel_probe_start(&p, 1000, 3);
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
    panel_probe_start(&p, 1000, 3);
    CHECK(strcmp(word(&p), "...") == 0, "running reads '%s'", word(&p));
    CHECK(panel_probe_step(&p, 1100, 1, true) == PANEL_PROBE_RUNNING, "one reply ended it");
    CHECK(panel_probe_step(&p, 1200, 3, true) == PANEL_PROBE_PASS, "three replies did not pass");
    CHECK(strcmp(word(&p), "3/3 OK") == 0, "reads '%s'", word(&p));
    CHECK(panel_probe_passed(&p) && panel_probe_settled(&p), "pass is not settled");
}

static void test_some_answers_is_partial(void)
{
    panel_probe_t p; panel_probe_init(&p);
    panel_probe_start(&p, 1000, 3);
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
    panel_probe_start(&p, t0, 3);
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
    panel_probe_start(&p, 1000, 3);
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
    panel_probe_start(&p, 0, 3);
    panel_probe_step(&p, 9999, 0, true);
    panel_probe_word(&p, buf, 4);
    CHECK(buf[3] == '\0', "a short buffer was overrun");
    CHECK(panel_probe_word(&p, NULL, 8)[0] == '\0', "a null buffer returned text");
}

int main(void)
{
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
