/* Host tests for exchange identity (E2E v0 slice 3, step 3.3b).
 *
 * The plan's gate: a result that arrives after each of the five retiring
 * events (GOODNIGHT, STOP, release, link loss, a new hold) produces NO STATE
 * CHANGE and NO OP. "No state change" is asserted on the whole struct, byte
 * for byte, and "no op" as panel_exchange_may_act() being false. That is the
 * only question D-032's reply door asks.
 *
 * The illegal cases come first, and every refusal is checked by its reason,
 * because a check that only asserts "not OK" still passes if the id test is
 * deleted and the phase test happens to refuse instead.
 */
#include <stdio.h>
#include <string.h>

#include "panel_exchange.h"

static int failures = 0, checks = 0;

#define CHECK(cond, ...) do {                                \
    checks++;                                                \
    if (!(cond)) { failures++;                               \
        printf("  FAIL: "); printf(__VA_ARGS__);             \
        printf("\n        at %s:%d\n", __FILE__, __LINE__); }\
} while (0)

#define VERDICT(got, want, what) \
    CHECK((got) == (want), "%s: got %s, want %s", (what), \
          panel_exchange_verdict_name(got), panel_exchange_verdict_name(want))

static const px_retire_t RETIRING[] = {
    PX_RETIRE_GOODNIGHT, PX_RETIRE_STOP, PX_RETIRE_RELEASE,
    PX_RETIRE_LINK_LOST, PX_RETIRE_NEW_HOLD,
};
#define N_RETIRING (sizeof RETIRING / sizeof RETIRING[0])

/* A retiring event, done the way the panel will do it: a new hold goes
 * through begin(), which is where that retirement actually happens. */
static void fire(panel_exchange_t *px, px_retire_t why)
{
    if (why == PX_RETIRE_NEW_HOLD) (void)panel_exchange_begin(px);
    else                           panel_exchange_retire(px, why);
}

static uint32_t to_thinking(panel_exchange_t *px)
{
    uint32_t id = panel_exchange_begin(px);
    panel_exchange_released(px, id);
    return id;
}

/* ---- illegal cases first ------------------------------------------------ */

static void test_zero_value_refuses_everything(void)
{
    panel_exchange_t px;
    memset(&px, 0, sizeof px);
    VERDICT(panel_exchange_released(&px, 0),   PX_DROP_NOT_LIVE, "released(0)");
    VERDICT(panel_exchange_transcript(&px, 0), PX_DROP_NOT_LIVE, "transcript(0)");
    VERDICT(panel_exchange_answer(&px, 0),     PX_DROP_NOT_LIVE, "answer(0)");
    CHECK(!panel_exchange_may_act(&px, 0), "zeroed struct may act for id 0");
    CHECK(!panel_exchange_may_act(&px, 1), "zeroed struct may act for id 1");
}

static void test_id_zero_is_never_live(void)
{
    /* live == 0 must not match id 0 -- the obvious bug in `id == live`. */
    panel_exchange_t px;
    panel_exchange_reset(&px);
    px.phase = PX_ANSWERING;  /* even with the phase forced */
    CHECK(!panel_exchange_may_act(&px, 0), "id 0 may act with nothing live");
    VERDICT(panel_exchange_answer(&px, 0), PX_DROP_NOT_LIVE, "answer(0), forced phase");
}

static void test_never_issued_id_is_dropped(void)
{
    panel_exchange_t px;
    panel_exchange_reset(&px);
    uint32_t id = to_thinking(&px);
    panel_exchange_transcript(&px, id);
    VERDICT(panel_exchange_answer(&px, id + 1), PX_DROP_NOT_LIVE, "answer(future id)");
    CHECK(px.phase == PX_THINKING, "a future id moved the phase");
}

static void test_results_out_of_phase_are_dropped(void)
{
    panel_exchange_t px;
    panel_exchange_reset(&px);
    uint32_t id = panel_exchange_begin(&px);

    VERDICT(panel_exchange_transcript(&px, id), PX_DROP_PHASE, "transcript while LISTENING");
    VERDICT(panel_exchange_answer(&px, id),     PX_DROP_PHASE, "answer while LISTENING");
    CHECK(!panel_exchange_may_act(&px, id), "may act while LISTENING");

    panel_exchange_released(&px, id);
    VERDICT(panel_exchange_released(&px, id), PX_DROP_PHASE, "released twice");
    VERDICT(panel_exchange_answer(&px, id), PX_DROP_PHASE, "answer before transcript");
    CHECK(px.phase == PX_THINKING, "answer before transcript moved the phase");
    CHECK(!panel_exchange_may_act(&px, id), "may act while THINKING");

    VERDICT(panel_exchange_transcript(&px, id), PX_OK, "first transcript");
    VERDICT(panel_exchange_transcript(&px, id), PX_DROP_PHASE, "second transcript");
    VERDICT(panel_exchange_answer(&px, id), PX_OK, "answer");
    VERDICT(panel_exchange_answer(&px, id), PX_DROP_PHASE, "second answer");
}

/* ---- the plan's gate: five events x the stage the result arrives in ----- */

static void test_result_after_each_retiring_event_changes_nothing(void)
{
    for (size_t i = 0; i < N_RETIRING; i++) {
        const char *ev = panel_exchange_retire_name(RETIRING[i]);

        /* A transcript that was in flight when the event fired. */
        panel_exchange_t px;
        panel_exchange_reset(&px);
        uint32_t old = to_thinking(&px);
        fire(&px, RETIRING[i]);
        panel_exchange_t before = px;
        VERDICT(panel_exchange_transcript(&px, old), PX_DROP_NOT_LIVE, ev);
        CHECK(memcmp(&before, &px, sizeof px) == 0,
              "late transcript after %s changed state", ev);
        CHECK(!panel_exchange_may_act(&px, old), "may act for old id after %s", ev);

        /* An answer that was in flight when the event fired. */
        panel_exchange_reset(&px);
        old = to_thinking(&px);
        panel_exchange_transcript(&px, old);
        fire(&px, RETIRING[i]);
        before = px;
        VERDICT(panel_exchange_answer(&px, old), PX_DROP_NOT_LIVE, ev);
        CHECK(memcmp(&before, &px, sizeof px) == 0,
              "late answer after %s changed state", ev);
        CHECK(!panel_exchange_may_act(&px, old), "may act for old id after %s", ev);
        CHECK(px.phase != PX_ANSWERING, "late answer after %s entered ANSWERING", ev);

        /* The event fired while he was already answering. */
        panel_exchange_reset(&px);
        old = to_thinking(&px);
        panel_exchange_transcript(&px, old);
        panel_exchange_answer(&px, old);
        CHECK(panel_exchange_may_act(&px, old), "legal answer cannot act (%s)", ev);
        fire(&px, RETIRING[i]);
        CHECK(!panel_exchange_may_act(&px, old),
              "the door stayed open after %s mid-answer", ev);
    }
}

static void test_retirement_records_its_reason(void)
{
    for (size_t i = 0; i < N_RETIRING; i++) {
        panel_exchange_t px;
        panel_exchange_reset(&px);
        (void)to_thinking(&px);
        fire(&px, RETIRING[i]);
        CHECK(px.last_retired == RETIRING[i], "retired as %s, recorded %s",
              panel_exchange_retire_name(RETIRING[i]),
              panel_exchange_retire_name(px.last_retired));
    }
}

static void test_a_new_hold_does_not_inherit_the_old_answer(void)
{
    /* The old exchange's answer must not land on the new one, which is live
     * and THINKING and would otherwise pass the phase test. */
    panel_exchange_t px;
    panel_exchange_reset(&px);
    uint32_t old = to_thinking(&px);
    panel_exchange_transcript(&px, old);
    uint32_t now = to_thinking(&px);
    panel_exchange_transcript(&px, now);
    CHECK(now != old, "a new hold reused the id");
    VERDICT(panel_exchange_answer(&px, old), PX_DROP_NOT_LIVE, "old answer, new exchange");
    CHECK(px.phase == PX_THINKING, "old answer moved the new exchange");
    VERDICT(panel_exchange_answer(&px, now), PX_OK, "the new exchange's own answer");
}

static void test_begin_clears_a_stale_transcript_flag(void)
{
    /* begin() is the only way into a live exchange, so it alone must clear
     * the flag. With nothing live, retire() returns early and cannot help. */
    panel_exchange_t px;
    panel_exchange_reset(&px);
    px.transcript_in = true;
    uint32_t id = to_thinking(&px);
    VERDICT(panel_exchange_answer(&px, id), PX_DROP_PHASE,
            "answer with a transcript flag left from before the hold");
}

/* ---- legal path, after the refusals ------------------------------------ */

static void test_the_whole_exchange(void)
{
    panel_exchange_t px;
    panel_exchange_reset(&px);
    uint32_t id = panel_exchange_begin(&px);
    CHECK(id != 0, "begin issued 0");
    CHECK(px.phase == PX_LISTENING, "begin did not listen");
    VERDICT(panel_exchange_released(&px, id),   PX_OK, "released");
    VERDICT(panel_exchange_transcript(&px, id), PX_OK, "transcript");
    CHECK(!panel_exchange_may_act(&px, id), "may act before the answer");
    VERDICT(panel_exchange_answer(&px, id),     PX_OK, "answer");
    CHECK(px.phase == PX_ANSWERING, "answer did not enter ANSWERING");
    CHECK(panel_exchange_may_act(&px, id), "cannot act while ANSWERING");
    panel_exchange_retire(&px, PX_RETIRE_DONE);
    CHECK(!panel_exchange_may_act(&px, id), "may act after DONE");
    CHECK(px.phase == PX_NONE && px.live == 0, "DONE left an exchange live");
}

static void test_ids_are_monotonic_and_skip_zero_on_wrap(void)
{
    panel_exchange_t px;
    panel_exchange_reset(&px);
    uint32_t a = panel_exchange_begin(&px);
    uint32_t b = panel_exchange_begin(&px);
    CHECK(b > a, "ids went %u then %u", (unsigned)a, (unsigned)b);

    px.last_issued = UINT32_MAX;
    uint32_t w = panel_exchange_begin(&px);
    CHECK(w == 1, "wrap issued %u, want 1", (unsigned)w);
}

static void test_retire_with_nothing_live_keeps_the_last_reason(void)
{
    panel_exchange_t px;
    panel_exchange_reset(&px);
    (void)panel_exchange_begin(&px);
    panel_exchange_retire(&px, PX_RETIRE_STOP);
    panel_exchange_retire(&px, PX_RETIRE_GOODNIGHT);
    CHECK(px.last_retired == PX_RETIRE_STOP,
          "a no-op retire overwrote the reason with %s",
          panel_exchange_retire_name(px.last_retired));
}

int main(void)
{
    test_zero_value_refuses_everything();
    test_id_zero_is_never_live();
    test_never_issued_id_is_dropped();
    test_results_out_of_phase_are_dropped();
    test_result_after_each_retiring_event_changes_nothing();
    test_retirement_records_its_reason();
    test_a_new_hold_does_not_inherit_the_old_answer();
    test_begin_clears_a_stale_transcript_flag();
    test_the_whole_exchange();
    test_ids_are_monotonic_and_skip_zero_on_wrap();
    test_retire_with_nothing_live_keeps_the_last_reason();

    printf("%s: %d checks, %d failures\n",
           failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
