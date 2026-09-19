/* Host tests for the telemetry surface (#114 AC6).
 *
 * The property under test is not "the struct holds numbers". It is that a
 * value CANNOT OUTLIVE THE LINK THAT CARRIED IT -- so nearly every test here
 * is about a reading going away, or refusing to be shown, rather than about
 * one arriving.
 */
#include <stdio.h>
#include <string.h>

#include "r2_telemetry.h"

static int failures = 0, checks = 0;

#define CHECK(cond, ...) do {                                \
    checks++;                                                \
    if (!(cond)) { failures++;                               \
        printf("  FAIL: "); printf(__VA_ARGS__);             \
        printf("\n        at %s:%d\n", __FILE__, __LINE__); }\
} while (0)

static r2_telemetry_t up_with_readings(uint32_t t0)
{
    r2_telemetry_t t;
    r2_telemetry_reset(&t);
    r2_telemetry_link(&t, R2_TM_UP, t0);
    r2_telemetry_battery(&t, 442, t0);
    r2_telemetry_dome(&t, 0.15f, t0);
    r2_telemetry_version(&t, 7, 0, 101, t0);
    return t;
}

/* ---- the rule ----------------------------------------------------------- */

static void test_readings_do_not_survive_the_link(void)
{
    /* Every non-UP state, because handling only DOWN is the obvious bug: a
     * reconnect goes UP -> SCANNING -> CONNECTING -> HANDSHAKING -> UP and
     * never visits DOWN at all. */
    const r2_tm_link_t fell_to[] = {
        R2_TM_DOWN, R2_TM_SCANNING, R2_TM_CONNECTING, R2_TM_HANDSHAKING
    };
    for (size_t i = 0; i < sizeof fell_to / sizeof fell_to[0]; i++) {
        r2_telemetry_t t = up_with_readings(1000);
        CHECK(t.battery.valid, "precondition: battery should be valid while up");

        r2_telemetry_link(&t, fell_to[i], 2000);
        CHECK(!t.battery.valid, "battery survived link -> %s",
              r2_telemetry_link_name(fell_to[i]));
        CHECK(!t.dome.valid, "dome survived link -> %s",
              r2_telemetry_link_name(fell_to[i]));
        CHECK(!t.version.valid, "version survived link -> %s",
              r2_telemetry_link_name(fell_to[i]));

        uint32_t age;
        CHECK(!r2_telemetry_age_ms(&t.battery, 2000, &age),
              "an invalidated reading still reported an age");
    }
}

/* A reconnect must not republish the previous session's numbers. This is
 * "assert the status on connect, never inherit it" applied to data. */
static void test_a_new_link_does_not_inherit_the_old_ones_readings(void)
{
    r2_telemetry_t t = up_with_readings(1000);
    r2_telemetry_link(&t, R2_TM_DOWN, 2000);
    r2_telemetry_link(&t, R2_TM_SCANNING, 2100);
    r2_telemetry_link(&t, R2_TM_UP, 3000);

    CHECK(!t.battery.valid, "a reconnect inherited the previous session's battery");
    CHECK(!t.dome.valid,    "a reconnect inherited the previous session's dome");
    CHECK(!t.version.valid, "a reconnect inherited the previous session's version");
    CHECK(!r2_telemetry_displayable(&t, &t.battery, 3000, 60000),
          "a freshly reconnected link offered a reading it has not taken yet");
}

/* Frames are in flight across a disconnect, so this is reachable, not
 * theoretical -- and storing one resurrects exactly what the transition just
 * forgot. */
static void test_a_reading_arriving_while_down_is_discarded(void)
{
    r2_telemetry_t t;
    r2_telemetry_reset(&t);

    r2_telemetry_battery(&t, 442, 500);
    CHECK(!t.battery.valid, "a battery reading was stored while the link was down");

    r2_telemetry_link(&t, R2_TM_HANDSHAKING, 600);
    r2_telemetry_battery(&t, 442, 700);
    CHECK(!t.battery.valid, "a reading was stored during handshaking");

    r2_telemetry_dome(&t, 12.0f, 700);
    CHECK(!t.dome.valid, "a dome reading was stored during handshaking");
    r2_telemetry_version(&t, 7, 0, 101, 700);
    CHECK(!t.version.valid, "a version was stored during handshaking");

    CHECK(t.responses == 0, "discarded readings were counted as responses");
}

static void test_nothing_is_displayable_when_the_link_is_not_up(void)
{
    r2_telemetry_t t = up_with_readings(1000);
    CHECK(r2_telemetry_displayable(&t, &t.battery, 1000, 60000),
          "precondition: a fresh reading on a live link should display");

    r2_telemetry_link(&t, R2_TM_DOWN, 1001);
    CHECK(!r2_telemetry_displayable(&t, &t.battery, 1001, 60000),
          "a reading was displayable with the link down -- this is the whole bug");
    CHECK(!r2_telemetry_displayable(&t, &t.dome, 1001, 60000), "dome displayable when down");
    CHECK(!r2_telemetry_displayable(&t, &t.version, 1001, 60000), "version displayable when down");
}

/* The link check inside displayable() is REDUNDANT with the invalidation that
 * happens on a link transition -- and a mutation deleting it survived the
 * first battery precisely because of that overlap: the reading was already
 * invalid, so the test passed through the other guard.
 *
 * It is kept rather than deleted because the two guards sit in different
 * places for different reasons. Invalidation is bookkeeping at the moment the
 * link changes; this is the check AT THE POINT OF USE, and this repo's rule is
 * that the guard belongs where the effect happens. r2_telemetry_t is a public
 * struct: a caller can hold one this module never transitioned, and a future
 * refactor could stop invalidating without anyone noticing this line was the
 * only thing left.
 *
 * So it gets a test that can only pass through IT -- a reading marked valid
 * beside a link that is not up, which the normal paths cannot produce. */
static void test_displayable_refuses_a_dead_link_even_if_the_reading_looks_valid(void)
{
    r2_telemetry_t t = up_with_readings(1000);

    /* Hand-forced: valid reading, link not up. Unreachable through the API,
     * which is the point -- this isolates the second guard from the first. */
    t.link = R2_TM_DOWN;
    CHECK(t.battery.valid, "precondition: the reading is still marked valid");
    CHECK(!r2_telemetry_displayable(&t, &t.battery, 1000, 60000),
          "displayable() showed a valid reading beside a DOWN link");

    t.link = R2_TM_SCANNING;
    CHECK(!r2_telemetry_displayable(&t, &t.battery, 1000, 60000),
          "displayable() showed a valid reading while scanning");
    t.link = R2_TM_HANDSHAKING;
    CHECK(!r2_telemetry_displayable(&t, &t.dome, 1000, 60000),
          "displayable() showed a valid reading while handshaking");

    t.link = R2_TM_UP;
    CHECK(r2_telemetry_displayable(&t, &t.battery, 1000, 60000),
          "displayable() refused a valid reading on a live link");
}

/* ---- staleness ---------------------------------------------------------- */

static void test_age_and_the_staleness_cutoff(void)
{
    r2_telemetry_t t = up_with_readings(10000);
    uint32_t age;

    CHECK(r2_telemetry_age_ms(&t.battery, 10000, &age) && age == 0, "age at t=0");
    CHECK(r2_telemetry_age_ms(&t.battery, 15000, &age) && age == 5000, "age after 5 s");

    CHECK(r2_telemetry_displayable(&t, &t.battery, 15000, 5000),
          "exactly at the cutoff must display (<=, not <)");
    CHECK(!r2_telemetry_displayable(&t, &t.battery, 15001, 5000),
          "one ms past the cutoff must not display");
    CHECK(r2_telemetry_displayable(&t, &t.battery, 14999, 5000), "just inside the cutoff");
}

/* 2^32 ms is ~49.7 days. A droid that lives in a household reaches it, and the
 * naive guard reports a wrapped reading as BRAND NEW -- the panel at its most
 * confident exactly when its clock rolled over. */
static void test_the_clock_wrap_at_49_days(void)
{
    r2_telemetry_t t;
    r2_telemetry_reset(&t);
    const uint32_t before_wrap = 0xFFFFF000u;
    r2_telemetry_link(&t, R2_TM_UP, before_wrap);
    r2_telemetry_battery(&t, 442, before_wrap);

    const uint32_t after_wrap = 0x00001000u;    /* 0x2000 ms later, wrapped */
    uint32_t age;
    CHECK(r2_telemetry_age_ms(&t.battery, after_wrap, &age),
          "no age reported across the wrap");
    CHECK(age == 0x2000u, "age across the wrap is %u ms, want %u", age, 0x2000u);
    CHECK(!r2_telemetry_displayable(&t, &t.battery, after_wrap, 1000),
          "a reading 8.2 s old across the wrap was shown as fresh");
}

static void test_age_refuses_when_there_is_nothing_to_age(void)
{
    r2_telemetry_t t;
    r2_telemetry_reset(&t);
    uint32_t age = 0xDEADBEEF;
    CHECK(!r2_telemetry_age_ms(&t.battery, 5000, &age),
          "reported an age for a reading that never happened");
    CHECK(age == 0xDEADBEEF, "clobbered the caller's variable on refusal");
    CHECK(!r2_telemetry_age_ms(NULL, 5000, &age), "null stamp accepted");
    r2_telemetry_t t2 = up_with_readings(1000);
    CHECK(!r2_telemetry_age_ms(&t2.battery, 1000, NULL), "null out accepted");
}

static void test_nulls(void)
{
    r2_telemetry_t t = up_with_readings(1000);
    r2_telemetry_reset(NULL);
    r2_telemetry_link(NULL, R2_TM_UP, 0);
    r2_telemetry_battery(NULL, 1, 0);
    r2_telemetry_dome(NULL, 1.0f, 0);
    r2_telemetry_version(NULL, 1, 1, 1, 0);
    r2_telemetry_note_request(NULL);
    CHECK(!r2_telemetry_displayable(NULL, &t.battery, 1000, 1000), "null telemetry");
    CHECK(!r2_telemetry_displayable(&t, NULL, 1000, 1000), "null stamp");
    checks++;  /* survived every null without crashing */
}

/* ---- how long he has been unreachable ----------------------------------- */

/* THE ILLEGAL CASE FIRST: a step inside one attempt must not restart the
 * clock. A connect that fails goes CONNECTING -> SCANNING without visiting
 * DOWN; if that reset the count, a droid that keeps half-answering would hold
 * the panel in `waking` indefinitely, which is the bug the bound exists to
 * end. */
static void test_a_failed_connect_does_not_restart_the_clock(void)
{
    r2_telemetry_t t = up_with_readings(1000);
    r2_telemetry_link(&t, R2_TM_DOWN,       2000);
    r2_telemetry_link(&t, R2_TM_SCANNING,   2000);   /* same callback */
    r2_telemetry_link(&t, R2_TM_CONNECTING, 2500);
    r2_telemetry_link(&t, R2_TM_SCANNING,   3000);   /* connect failed */
    r2_telemetry_link(&t, R2_TM_CONNECTING, 3500);
    r2_telemetry_link(&t, R2_TM_HANDSHAKING, 3600);
    CHECK(r2_telemetry_unreachable_ms(&t, 6000) == 4000,
          "unreachable %u ms, want 4000 -- a step restarted the attempt",
          r2_telemetry_unreachable_ms(&t, 6000));
}

/* THE CASE THE FIRST VERSION GOT WRONG, found in review: a connection that
 * is made and then drops mid-handshake goes back through DOWN. Restarting the
 * clock on leaving DOWN made a droid that connects and drops every 1.5 s read
 * ~100 ms unreachable indefinitely, and OFFLINE never came. */
static void test_a_dropped_handshake_does_not_restart_the_clock(void)
{
    r2_telemetry_t t = up_with_readings(1000);
    uint32_t now = 2000;
    r2_telemetry_link(&t, R2_TM_DOWN,     now);   /* he drops */
    r2_telemetry_link(&t, R2_TM_SCANNING, now);
    for (int i = 0; i < 5; i++) {                 /* five near-misses */
        r2_telemetry_link(&t, R2_TM_CONNECTING,  now + 300);
        r2_telemetry_link(&t, R2_TM_HANDSHAKING, now + 600);
        r2_telemetry_link(&t, R2_TM_DOWN,        now + 1500);
        r2_telemetry_link(&t, R2_TM_SCANNING,    now + 1500);
        now += 1500;
        CHECK(r2_telemetry_unreachable_ms(&t, now + 100) == now + 100 - 2000,
              "near-miss %d: unreachable %u ms, want %u -- a dropped handshake "
              "restarted the attempt", i + 1,
              r2_telemetry_unreachable_ms(&t, now + 100), now + 100 - 2000);
    }
}

/* And UP is what closes an attempt, so the NEXT loss starts a fresh one
 * rather than inheriting the last one's age. */
static void test_having_him_back_closes_the_attempt(void)
{
    r2_telemetry_t t = up_with_readings(1000);
    r2_telemetry_link(&t, R2_TM_SCANNING, 2000);
    r2_telemetry_link(&t, R2_TM_UP,       5000);
    r2_telemetry_link(&t, R2_TM_SCANNING, 90000);
    CHECK(r2_telemetry_unreachable_ms(&t, 90500) == 500,
          "second loss: unreachable %u ms, want 500 -- it inherited the first",
          r2_telemetry_unreachable_ms(&t, 90500));
}

/* How long he has been GONE is only knowable if we saw him go. A boot's
 * attempt measures how long we have been looking; a drop's measures his
 * absence. The panel must be able to tell them apart. */
static void test_an_attempt_knows_whether_it_lost_him(void)
{
    r2_telemetry_t t;
    r2_telemetry_reset(&t);
    r2_telemetry_link(&t, R2_TM_SCANNING, 900);
    CHECK(!t.attempt_from_up, "a boot attempt claimed to have lost a live link");
    r2_telemetry_link(&t, R2_TM_UP, 3000);
    r2_telemetry_link(&t, R2_TM_DOWN, 9000);
    r2_telemetry_link(&t, R2_TM_SCANNING, 9000);
    CHECK(t.attempt_from_up, "a drop from a live link was not recorded as one");
    /* A dropped handshake mid-attempt does not rewrite how it began. */
    r2_telemetry_link(&t, R2_TM_CONNECTING, 9500);
    r2_telemetry_link(&t, R2_TM_DOWN, 9800);
    r2_telemetry_link(&t, R2_TM_SCANNING, 9800);
    CHECK(t.attempt_from_up, "a mid-attempt DOWN rewrote how the attempt began");
    /* And a reset forgets it: the next boot has not lost anyone. */
    r2_telemetry_reset(&t);
    CHECK(!t.attempt_from_up, "a reset kept the previous attempt's origin");
}

/* A reconnect can leave UP for SCANNING without ever visiting DOWN; the
 * attempt starts there all the same. */
static void test_leaving_up_by_any_route_starts_the_clock(void)
{
    r2_telemetry_t t = up_with_readings(1000);
    r2_telemetry_link(&t, R2_TM_SCANNING, 5000);
    CHECK(r2_telemetry_unreachable_ms(&t, 6000) == 1000,
          "UP -> SCANNING: unreachable %u ms, want 1000",
          r2_telemetry_unreachable_ms(&t, 6000));
}

static void test_a_live_link_is_not_unreachable(void)
{
    r2_telemetry_t t = up_with_readings(1000);
    CHECK(r2_telemetry_unreachable_ms(&t, 900000) == 0,
          "a live link reported %u ms unreachable",
          r2_telemetry_unreachable_ms(&t, 900000));
    CHECK(r2_telemetry_unreachable_ms(NULL, 900000) == 0, "null telemetry");
}

/* Boot is timed from the first SCAN, not from power-on -- the interval P2
 * measured. Before the host syncs there is no scan, and the time since reset
 * is the honest answer: a radio that never comes up is unreachable, not
 * forever about to connect. */
static void test_boot_is_timed_from_the_first_scan(void)
{
    r2_telemetry_t t;
    r2_telemetry_reset(&t);
    CHECK(r2_telemetry_unreachable_ms(&t, 800) == 800,
          "before the host synced: %u ms, want the time since reset",
          r2_telemetry_unreachable_ms(&t, 800));

    r2_telemetry_link(&t, R2_TM_SCANNING, 900);
    CHECK(r2_telemetry_unreachable_ms(&t, 1000) == 100,
          "after the first scan: %u ms, want 100 -- boot was billed to R2",
          r2_telemetry_unreachable_ms(&t, 1000));
}

static void test_unreachable_across_the_wrap(void)
{
    r2_telemetry_t t = up_with_readings(1000);
    r2_telemetry_link(&t, R2_TM_SCANNING, 0xFFFFF000u);
    CHECK(r2_telemetry_unreachable_ms(&t, 0x00001000u) == 0x2000u,
          "across the wrap: %u ms, want %u",
          r2_telemetry_unreachable_ms(&t, 0x00001000u), 0x2000u);
}

/* ---- values and accounting ---------------------------------------------- */

static void test_values_are_carried(void)
{
    r2_telemetry_t t = up_with_readings(1000);
    CHECK(t.battery_centivolts == 442, "battery %u", t.battery_centivolts);
    CHECK(t.dome_degrees > 0.14f && t.dome_degrees < 0.16f, "dome %f", (double)t.dome_degrees);
    CHECK(t.version_major == 7 && t.version_minor == 0 && t.version_revision == 101,
          "version %u.%u.%u", t.version_major, t.version_minor, t.version_revision);
    CHECK(t.link_since_ms == 1000, "link_since %u", t.link_since_ms);

    /* A repeated identical value must still refresh the age -- otherwise a
     * battery that reads 4.42 V for an hour (which is what a charging droid
     * does) would age out and be hidden while it is being answered perfectly. */
    r2_telemetry_battery(&t, 442, 50000);
    uint32_t age;
    CHECK(r2_telemetry_age_ms(&t.battery, 50000, &age) && age == 0,
          "an unchanged value did not refresh its timestamp");
}

/* The ratio nobody was displaying is what hid the escaping bug for a whole
 * endurance run. */
static void test_request_response_accounting(void)
{
    r2_telemetry_t t;
    r2_telemetry_reset(&t);
    r2_telemetry_link(&t, R2_TM_UP, 0);
    for (int i = 0; i < 10; i++) r2_telemetry_note_request(&t);
    for (int i = 0; i < 8; i++)  r2_telemetry_battery(&t, 442, 100);
    r2_telemetry_note_refused(&t);
    r2_telemetry_note_dropped(&t);
    r2_telemetry_note_dropped(&t);

    CHECK(t.requests == 10, "requests %u", t.requests);
    CHECK(t.responses == 8, "responses %u", t.responses);
    CHECK(t.refused == 1, "refused %u", t.refused);
    CHECK(t.dropped == 2, "dropped %u", t.dropped);

    /* answered must never exceed asked. A ratio that can go above 1 is worse
     * than no ratio -- it makes the one number that would have exposed the
     * escaping bug look like a broken counter instead of a finding. Caught on
     * hardware: the version probe was answered and never counted as asked, and
     * the panel line read "asked 2 answered 3". */
    CHECK(t.responses <= t.requests,
          "answered (%u) exceeds asked (%u) -- every response needs a counted request",
          t.responses, t.requests);

    /* Counters are session accounting, not readings: they describe what WE
     * did, so a link drop must not erase them. */
    r2_telemetry_link(&t, R2_TM_DOWN, 200);
    CHECK(t.requests == 10 && t.responses == 8,
          "a link drop erased the request accounting");
}

/* ---- released (D-023 GOODNIGHT, E2E v0 slice 1) ----------------------- */

static void test_a_release_opens_no_attempt(void)
{
    r2_telemetry_t t = up_with_readings(1000);
    r2_telemetry_released(&t, true, 2000);
    r2_telemetry_link(&t, R2_TM_DOWN, 2100);      /* the disconnect lands */
    CHECK(!t.attempt_open, "releasing him opened an attempt to reach him");
}

static void test_waking_after_a_long_release_is_timed_from_the_wake(void)
{
    /* THE BUG THIS EXISTS FOR: released for an hour, then WAKE. Timed from
     * the release, the panel would read an hour away and show OFFLINE the
     * instant the button was pressed. */
    r2_telemetry_t t = up_with_readings(1000);
    r2_telemetry_released(&t, true, 2000);
    r2_telemetry_link(&t, R2_TM_DOWN, 2100);
    const uint32_t woke = 2000 + 3600000u;
    r2_telemetry_released(&t, false, woke);
    r2_telemetry_link(&t, R2_TM_SCANNING, woke + 5);
    CHECK(t.attempt_open, "WAKE did not open an attempt");
    CHECK(r2_telemetry_unreachable_ms(&t, woke + 1000) == 1000,
          "wake timed from the release: %u ms",
          (unsigned)r2_telemetry_unreachable_ms(&t, woke + 1000));
    CHECK(!t.attempt_from_up, "a wake was counted as losing a live link");
}

static void test_a_wake_attempt_still_closes_on_up(void)
{
    r2_telemetry_t t;
    r2_telemetry_reset(&t);
    r2_telemetry_released(&t, true, 0);
    r2_telemetry_released(&t, false, 100);
    r2_telemetry_link(&t, R2_TM_UP, 2800);
    CHECK(!t.attempt_open, "reaching him did not close the wake attempt");
}

static void test_releasing_twice_is_one_release(void)
{
    r2_telemetry_t t;
    r2_telemetry_reset(&t);
    r2_telemetry_released(&t, false, 0);     /* already not released: no-op */
    CHECK(!t.attempt_open, "clearing a flag that was never set opened an attempt");
}

int main(void)
{
    printf("r2_telemetry host tests\n");
    printf("  a value must not outlive its link\n");
    test_readings_do_not_survive_the_link();
    test_a_new_link_does_not_inherit_the_old_ones_readings();
    test_a_reading_arriving_while_down_is_discarded();
    test_nothing_is_displayable_when_the_link_is_not_up();
    test_displayable_refuses_a_dead_link_even_if_the_reading_looks_valid();
    printf("  staleness\n");
    test_age_and_the_staleness_cutoff();
    test_the_clock_wrap_at_49_days();
    test_age_refuses_when_there_is_nothing_to_age();
    test_nulls();
    printf("  unreachable\n");
    test_a_failed_connect_does_not_restart_the_clock();
    test_a_dropped_handshake_does_not_restart_the_clock();
    test_having_him_back_closes_the_attempt();
    test_an_attempt_knows_whether_it_lost_him();
    test_leaving_up_by_any_route_starts_the_clock();
    test_a_live_link_is_not_unreachable();
    test_boot_is_timed_from_the_first_scan();
    test_unreachable_across_the_wrap();
    printf("  values\n");
    test_values_are_carried();
    test_request_response_accounting();
    printf("  released\n");
    test_a_release_opens_no_attempt();
    test_waking_after_a_long_release_is_timed_from_the_wake();
    test_a_wake_attempt_still_closes_on_up();
    test_releasing_twice_is_one_release();

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
