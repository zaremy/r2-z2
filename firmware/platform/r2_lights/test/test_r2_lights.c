/* Host tests for the board's status lights (E2E v0 slice 2).
 *
 * The port is compared against frames the PYTHON generated (golden.h), never
 * against a second copy of its own table. Refusals and the player's guards
 * come first.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "r2_lights.h"
#include "golden.h"

static int failures = 0, checks = 0;

#define CHECK(cond, ...) do {                                \
    checks++;                                                \
    if (!(cond)) { failures++;                               \
        printf("  FAIL: "); printf(__VA_ARGS__);             \
        printf("\n        at %s:%d\n", __FILE__, __LINE__); }\
} while (0)

static const r2_lights_frame_t SENTINEL = {{0xAA,0xAA,0xAA,0xAA,0xAA,0xAA,0xAA,0xAA}};

/* ---- ILLEGAL FIRST ------------------------------------------------------- */

static void test_no_row_is_refused(void)
{
    printf("     a state with no row samples nothing and leaves the frame alone\n");
    const int bad[] = { R2L_NONE, R2L__COUNT, 99, -7 };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        r2_lights_frame_t f = SENTINEL;
        CHECK(!r2_lights_sample((r2_lights_state_t)bad[i], 0, 1.0, &f),
              "state %d must not sample", bad[i]);
        CHECK(memcmp(&f, &SENTINEL, sizeof f) == 0, "state %d wrote the frame", bad[i]);
    }
    CHECK(!r2_lights_sample(R2L_IDLE, 0, 1.0, NULL), "a NULL frame must be refused");
}

static void test_an_idle_player_writes_nothing(void)
{
    printf("     a player nobody has entered owes nothing, for ever\n");
    r2_lights_player_t p;
    r2_lights_player_init(&p);
    r2_lights_frame_t f = SENTINEL;
    int writes = 0;
    for (uint32_t t = 0; t < 5000; t += 7)
        if (r2_lights_player_step(&p, t, 1.0, &f)) writes++;
    CHECK(writes == 0, "an un-entered player wrote %d times", writes);
    r2_lights_player_enter(&p, R2L_NONE, 10);
    CHECK(!r2_lights_player_step(&p, 5000, 1.0, &f), "entering NONE must not owe a write");
}

/* Drive a player with a step every millisecond, recording every write. */
typedef struct { uint32_t at; r2_lights_frame_t f; } write_t;
static int run(r2_lights_player_t *p, uint32_t from, uint32_t until,
               write_t *log, int cap)
{
    int n = 0;
    r2_lights_frame_t f;
    for (uint32_t t = from; t != until; t++) {
        if (r2_lights_player_step(p, t, 1.0, &f)) {
            if (n < cap) { log[n].at = t; log[n].f = f; }
            n++;
            r2_lights_player_sent(p, &f, t);
        }
    }
    return n;
}

static void test_spacing_is_never_broken(void)
{
    printf("     no two writes closer than 120 ms, in any state, with state changes mid-flight\n");
    static write_t log[4096];
    for (int s = 0; s < R2L__COUNT; s++) {
        r2_lights_player_t p;
        r2_lights_player_init(&p);
        r2_lights_player_enter(&p, (r2_lights_state_t)s, 0);
        int n = run(&p, 0, 4000, log, 4096);
        /* and a change of state every 37 ms, which is the worst a caller can do */
        for (uint32_t t = 4000; t < 6000; t += 37) {
            r2_lights_player_enter(&p, (r2_lights_state_t)((s + t) % R2L__COUNT), t);
            n += run(&p, t, t + 37, log + (n < 4096 ? n : 4095), 4096 - (n < 4096 ? n : 4095));
        }
        int bad = 0;
        for (int i = 1; i < n && i < 4096; i++)
            if (log[i].at - log[i - 1].at < R2_LIGHTS_MIN_INTERVAL_MS) bad++;
        CHECK(bad == 0, "%s: %d writes closer than %u ms", r2_lights_name(s), bad,
              R2_LIGHTS_MIN_INTERVAL_MS);
    }
}

/* ---- the port matches the source ----------------------------------------- */

static void test_every_state_matches_the_python(void)
{
    printf("     every state, both brightnesses, every 25 ms to 7 s, equals r2_lights.py\n");
    CHECK(GOLDEN_STATES == R2L__COUNT, "golden has %d states, C has %d",
          GOLDEN_STATES, R2L__COUNT);
    for (int s = 0; s < GOLDEN_STATES && s < R2L__COUNT; s++) {
        CHECK(strcmp(GOLDEN_NAME[s], r2_lights_name((r2_lights_state_t)s)) == 0,
              "row %d is %s in Python and %s in C", s, GOLDEN_NAME[s],
              r2_lights_name((r2_lights_state_t)s));
        int mismatches = 0, first = -1;
        for (int v = 0; v < GOLDEN_VALUES; v++)
            for (int i = 0; i < GOLDEN_SAMPLES; i++) {
                r2_lights_frame_t f;
                r2_lights_sample((r2_lights_state_t)s, (uint32_t)(i * GOLDEN_STEP_MS),
                                 GOLDEN_VALUE[v], &f);
                if (memcmp(f.v, GOLDEN[s][v][i], 8) != 0) {
                    if (first < 0) first = i * GOLDEN_STEP_MS;
                    mismatches++;
                }
            }
        CHECK(mismatches == 0, "%s: %d samples differ from Python, first at %d ms",
              GOLDEN_NAME[s], mismatches, first);
    }
}

static void test_the_golden_is_not_vacuous(void)
{
    printf("     the golden actually varies: blinks go dark, the sweep moves, dim dims\n");
    /* A generator that emitted zeros everywhere would make the test above
     * pass against a C table of zeros. */
    int dark = 0, lit = 0;
    for (int i = 0; i < GOLDEN_SAMPLES; i++) {
        if (GOLDEN[R2L_DANGER][0][i][0] == 0) dark++; else lit++;
    }
    CHECK(dark > 20 && lit > 20, "danger front red: %d dark, %d lit", dark, lit);
    CHECK(GOLDEN[R2L_WAKE][0][0][2] == 255 && GOLDEN[R2L_WAKE][0][GOLDEN_SAMPLES - 1][1] == 255,
          "wake must start blue and finish green");
    CHECK(GOLDEN[R2L_IDLE][0][0][2] == 255 && GOLDEN[R2L_IDLE][1][0][2] == 64,
          "idle dims 255 -> 64 at a quarter (got %d, %d)",
          GOLDEN[R2L_IDLE][0][0][2], GOLDEN[R2L_IDLE][1][0][2]);
    CHECK(GOLDEN[R2L_DANGER][1][0][0] == 255, "danger must NOT dim");
    CHECK(GOLDEN[R2L_SLEEP][0][0][2] == 51, "sleep is blue at 0.2 (got %d)",
          GOLDEN[R2L_SLEEP][0][0][2]);
}

/* ---- the player ---------------------------------------------------------- */

static void test_entering_asserts_even_when_nothing_changed(void)
{
    printf("     entering a state is an assertion: it writes even if that frame is already lit\n");
    r2_lights_player_t p;
    r2_lights_player_init(&p);
    r2_lights_frame_t f;
    r2_lights_player_enter(&p, R2L_IDLE, 0);
    CHECK(r2_lights_player_step(&p, 0, 1.0, &f), "idle must write on entry");
    r2_lights_player_sent(&p, &f, 0);
    CHECK(!r2_lights_player_step(&p, 60000, 1.0, &f), "a steady state must not re-write");

    r2_lights_player_forget(&p);
    CHECK(r2_lights_player_step(&p, 60001, 1.0, &f),
          "after forget, the same frame must be written again (a sleep may have wiped it)");
    r2_lights_player_sent(&p, &f, 60001);

    r2_lights_player_enter(&p, R2L_IDLE, 70000);
    CHECK(!r2_lights_player_step(&p, 70000, 1.0, &f),
          "re-entering the CURRENT state is a no-op, so callers may enter every tick");
}

static void test_a_held_write_sends_what_is_current(void)
{
    printf("     a write held back by the spacing sends the frame current when it goes\n");
    r2_lights_player_t p;
    r2_lights_player_init(&p);
    r2_lights_frame_t f;
    r2_lights_player_enter(&p, R2L_IDLE, 0);
    CHECK(r2_lights_player_step(&p, 0, 1.0, &f), "idle writes at 0");
    r2_lights_player_sent(&p, &f, 0);
    r2_lights_player_enter(&p, R2L_THINKING, 50);
    CHECK(!r2_lights_player_step(&p, 50, 1.0, &f), "50 ms after a write is too soon");
    CHECK(!r2_lights_player_step(&p, 119, 1.0, &f), "119 ms is still too soon");
    CHECK(r2_lights_player_step(&p, 120, 1.0, &f), "120 ms is soon enough");
    r2_lights_frame_t want;
    r2_lights_sample(R2L_THINKING, 70, 1.0, &want);
    CHECK(memcmp(&f, &want, sizeof f) == 0, "it must send thinking at 70 ms in, not a stale frame");
}

static void test_every_write_zeroes_what_the_state_does_not_use(void)
{
    printf("     leaving wake for idle turns the holo and logic OFF, explicitly\n");
    r2_lights_player_t p;
    r2_lights_player_init(&p);
    r2_lights_frame_t f;
    r2_lights_player_enter(&p, R2L_WAKE, 0);
    run(&p, 0, 2000, NULL, 0);
    CHECK(p.last.v[7] == 255 && p.last.v[3] == 255, "wake ends with holo and logic lit");
    r2_lights_player_enter(&p, R2L_IDLE, 2000);
    CHECK(r2_lights_player_step(&p, 2000, 1.0, &f), "idle must write");
    CHECK(f.v[7] == 0 && f.v[3] == 0 && f.v[2] == 255 && f.v[6] == 255,
          "idle must be blue with holo and logic dark (holo %d logic %d)", f.v[7], f.v[3]);
}

static void test_a_blink_survives_the_spacing(void)
{
    printf("     danger's 125 ms half-beat still renders both halves at 120 ms spacing\n");
    static write_t log[256];
    r2_lights_player_t p;
    r2_lights_player_init(&p);
    r2_lights_player_enter(&p, R2L_DANGER, 0);
    const int n = run(&p, 0, 2000, log, 256);
    int dark = 0, lit = 0;
    for (int i = 0; i < n && i < 256; i++) { if (log[i].f.v[0]) lit++; else dark++; }
    CHECK(n == 16 && dark == 8 && lit == 8,
          "2 s of danger: %d writes, %d lit, %d dark (want 16, 8, 8)", n, lit, dark);
}

static void test_a_failed_write_is_retried_at_the_spacing(void)
{
    printf("     a failed write is owed again, but not before 120 ms\n");
    r2_lights_player_t p;
    r2_lights_player_init(&p);
    r2_lights_frame_t f;
    r2_lights_player_enter(&p, R2L_IDLE, 0);
    CHECK(r2_lights_player_step(&p, 0, 1.0, &f), "idle writes on entry");
    r2_lights_player_failed(&p, 0);
    int early = 0;
    for (uint32_t t = 1; t < R2_LIGHTS_MIN_INTERVAL_MS; t++)
        if (r2_lights_player_step(&p, t, 1.0, &f)) early++;
    CHECK(early == 0, "a failed write was retried %d times inside the spacing", early);
    CHECK(r2_lights_player_step(&p, R2_LIGHTS_MIN_INTERVAL_MS, 1.0, &f),
          "a steady state whose write failed must still be owed");
    r2_lights_player_sent(&p, &f, R2_LIGHTS_MIN_INTERVAL_MS);
    CHECK(!r2_lights_player_step(&p, 5000, 1.0, &f), "once sent, it is not owed again");

    /* THE CASE THAT NEEDS THE FLAG: a re-assertion (forget) whose write fails,
     * of the very frame already recorded as lit. Without `owes_write` the
     * frame-compare alone says nothing changed, and it is never retried. */
    r2_lights_player_forget(&p);
    CHECK(r2_lights_player_step(&p, 6000, 1.0, &f), "a forgotten state is owed");
    r2_lights_player_failed(&p, 6000);
    CHECK(r2_lights_player_step(&p, 6000 + R2_LIGHTS_MIN_INTERVAL_MS, 1.0, &f),
          "a failed re-assertion of an unchanged frame must still be retried");
}

static void test_a_blink_survives_the_real_tick(void)
{
    printf("     at the board's 20 ms tick, danger still shows both halves every beat\n");
    /* The 1 ms test above is the player's best case; the board steps every
     * 20 ms, so this is the case that ships. */
    r2_lights_player_t p;
    r2_lights_player_init(&p);
    r2_lights_player_enter(&p, R2L_DANGER, 0);
    r2_lights_frame_t f;
    int lit = 0, dark = 0, worst = 0;
    uint32_t last = 0;
    for (uint32_t t = 0; t < 4000; t += 20) {
        if (!r2_lights_player_step(&p, t, 1.0, &f)) continue;
        if (t && (int)(t - last) > worst) worst = (int)(t - last);
        last = t;
        if (f.v[0]) lit++; else dark++;
        r2_lights_player_sent(&p, &f, t);
    }
    CHECK(lit >= 14 && dark >= 14, "4 s of danger at 20 ms: %d lit, %d dark", lit, dark);
    CHECK(worst <= 140, "the widest gap between writes was %d ms", worst);
}

static void test_the_clock_may_wrap(void)
{
    printf("     a clock that wraps between writes still measures the gap\n");
    r2_lights_player_t p;
    r2_lights_player_init(&p);
    r2_lights_frame_t f;
    const uint32_t near = UINT32_MAX - 50;
    r2_lights_player_enter(&p, R2L_IDLE, near);
    CHECK(r2_lights_player_step(&p, near, 1.0, &f), "writes on entry");
    r2_lights_player_sent(&p, &f, near);
    /* BEFORE the clock wraps, while last+120 already has: a comparison of
     * `now < last + 120` reads that as long past and writes 30 ms after the last one. */
    r2_lights_player_enter(&p, R2L_ATTENTION, near + 10);
    CHECK(!r2_lights_player_step(&p, near + 30, 1.0, &f), "30 ms before the wrap is too soon");
    CHECK(!r2_lights_player_step(&p, 20, 1.0, &f), "71 ms across the wrap is too soon");
    CHECK(r2_lights_player_step(&p, 70, 1.0, &f), "121 ms across the wrap is enough");
}

int main(void)
{
    printf("r2_lights host tests\n====================\n");
    test_no_row_is_refused();
    test_an_idle_player_writes_nothing();
    test_spacing_is_never_broken();
    test_every_state_matches_the_python();
    test_the_golden_is_not_vacuous();
    test_entering_asserts_even_when_nothing_changed();
    test_a_held_write_sends_what_is_current();
    test_every_write_zeroes_what_the_state_does_not_use();
    test_a_blink_survives_the_spacing();
    test_a_failed_write_is_retried_at_the_spacing();
    test_a_blink_survives_the_real_tick();
    test_the_clock_may_wrap();
    printf("====================\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
