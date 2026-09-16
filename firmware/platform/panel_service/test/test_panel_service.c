/* Host tests for the SERVICE interiors (#101 child 6: AC6, AC7).
 *
 * Illegal cases first, as elsewhere in this repo: a reading offered beside a
 * dead link, a ceiling that is corrupt, a character the font cannot draw.
 * The shipped rows being sensible today is not the property; the guard that
 * refuses the next bad one is. */
#include <stdio.h>
#include <string.h>

#include "panel_service.h"

static int failures = 0, checks = 0;
#define CHECK(cond, ...) do {                                \
    checks++;                                                \
    if (!(cond)) { failures++;                               \
        printf("  FAIL: "); printf(__VA_ARGS__);             \
        printf("\n        at %s:%d\n", __FILE__, __LINE__); }\
} while (0)

static r2_telemetry_t up_with_everything(uint32_t t0)
{
    r2_telemetry_t t;
    r2_telemetry_reset(&t);
    r2_telemetry_link(&t, R2_TM_UP, t0);
    r2_telemetry_note_request(&t);
    r2_telemetry_note_request(&t);
    r2_telemetry_battery(&t, 442, t0 + 100);
    r2_telemetry_version(&t, 7, 0, 101, t0 + 200);
    return t;
}

static panel_svc_facts_t facts(const r2_telemetry_t *tm, uint32_t now)
{
    panel_svc_facts_t f = {
        .tm = tm, .now_ms = now, .keepalive_ms = 3000, .ceiling = 0,
        .panel_fw = "b7a3e24-dirty", .flash_mb = 16, .psram_mb = 8, .heap_kb = 212,
        .touch_id = 0xB7,
    };
    return f;
}

static const panel_kv_t *row(const panel_kv_t *kv, int n, const char *key)
{
    for (int i = 0; i < n; i++) if (strcmp(kv[i].key, key) == 0) return &kv[i];
    return NULL;
}

/* ---- AC7: nothing R2 said survives the link ------------------------------ */

static void test_a_dead_link_shows_none_of_what_he_said(void)
{
    /* Every non-UP state, with readings HAND-FORCED valid -- the case the
     * normal paths cannot produce, so it can only pass through the check at
     * the point of use. */
    const r2_tm_link_t down[] = { R2_TM_DOWN, R2_TM_SCANNING,
                                  R2_TM_CONNECTING, R2_TM_HANDSHAKING };
    for (unsigned i = 0; i < sizeof down / sizeof down[0]; i++) {
        r2_telemetry_t t = up_with_everything(1000);
        t.link = down[i];                       /* readings still marked valid */
        panel_svc_facts_t f = facts(&t, 2000);
        panel_kv_t kv[PANEL_SVC_MAX_ROWS];
        const int n = panel_service_rows(PANEL_SVC_R2_LINK, &f, kv, PANEL_SVC_MAX_ROWS);
        const char *gone[] = { "LAST READ", "R2 FW", "KEEPALIVE" };
        for (unsigned g = 0; g < 3; g++) {
            const panel_kv_t *r = row(kv, n, gone[g]);
            CHECK(r && strcmp(r->val, "----") == 0 && r->tone == PANEL_TONE_NONE,
                  "link %s: %s reads '%s'", r2_telemetry_link_name(down[i]),
                  gone[g], r ? r->val : "(missing)");
        }
        const panel_kv_t *s = row(kv, n, "STATE");
        CHECK(s && strcmp(s->val, "LINKED") != 0 && s->tone == PANEL_TONE_WARN,
              "link %s: STATE claims '%s'", r2_telemetry_link_name(down[i]),
              s ? s->val : "(missing)");
        CHECK(panel_service_link_tone(&t) == PANEL_TONE_WARN,
              "link %s: the menu square is not amber", r2_telemetry_link_name(down[i]));
    }
}

static void test_accounting_survives_the_link(void)
{
    /* The one R2 LINK row that must NOT go blank: it counts what WE sent. */
    r2_telemetry_t t = up_with_everything(1000);
    r2_telemetry_link(&t, R2_TM_SCANNING, 5000);
    panel_svc_facts_t f = facts(&t, 6000);
    panel_kv_t kv[PANEL_SVC_MAX_ROWS];
    const int n = panel_service_rows(PANEL_SVC_R2_LINK, &f, kv, PANEL_SVC_MAX_ROWS);
    const panel_kv_t *a = row(kv, n, "ANSWERED");
    CHECK(a && strcmp(a->val, "2/2") == 0, "ANSWERED reads '%s'", a ? a->val : "(missing)");
}

static void test_a_live_link_says_what_he_said(void)
{
    r2_telemetry_t t = up_with_everything(1000);
    panel_svc_facts_t f = facts(&t, 13200);
    panel_kv_t kv[PANEL_SVC_MAX_ROWS];
    const int n = panel_service_rows(PANEL_SVC_R2_LINK, &f, kv, PANEL_SVC_MAX_ROWS);
    const struct { const char *k, *v; panel_tone_t tone; } want[] = {
        { "STATE",     "LINKED",   PANEL_TONE_GOOD  },
        { "CEILING",   "READ",     PANEL_TONE_PLAIN },
        { "ANSWERED",  "2/2",      PANEL_TONE_PLAIN },
        { "KEEPALIVE", "3S",       PANEL_TONE_PLAIN },
        { "LAST READ", "12S AGO",  PANEL_TONE_PLAIN },  /* newest: version at 1200 */
        { "R2 FW",     "7.0.101",  PANEL_TONE_PLAIN },
    };
    for (unsigned i = 0; i < sizeof want / sizeof want[0]; i++) {
        const panel_kv_t *r = row(kv, n, want[i].k);
        CHECK(r && strcmp(r->val, want[i].v) == 0 && r->tone == want[i].tone,
              "%s: '%s' tone %d, want '%s' tone %d", want[i].k,
              r ? r->val : "(missing)", r ? (int)r->tone : -1, want[i].v, (int)want[i].tone);
    }
    CHECK(panel_service_link_tone(&t) == PANEL_TONE_GOOD, "linked but the square is not green");
    CHECK(panel_service_link_tone(NULL) == PANEL_TONE_WARN, "no telemetry read as linked");
}

/* ---- AC6: the ladder ------------------------------------------------------ */

/* Every tier exercised. The tests below this line predate the sequence gate
 * (#168) and are about the CEILING; they say so explicitly rather than
 * passing 0 and quietly measuring the new gate instead of the old one. */
#define ALL_RUN 0x1Fu   /* READ..STANCE. Bit 5 would mark LOCOMOTION run,
                         * which the product can never reach -- a fixture the
                         * code under test cannot be handed in life. */

static void test_a_corrupt_ceiling_locks_everything(void)
{
    /* THE ILLEGAL CASE: clamping a bad ceiling to the nearest rung would draw
     * legs as allowed for a ceiling of 99. */
    const int bad[] = { -1, 5, 6, 99, -2147483647 - 1 };
    for (unsigned b = 0; b < sizeof bad / sizeof bad[0]; b++) {
        panel_rung_t r[PANEL_LADDER_RUNGS];
        panel_service_ladder(bad[b], ALL_RUN, r);
        for (int i = 0; i < PANEL_LADDER_RUNGS; i++)
            CHECK(!r[i].allowed, "ceiling %d allowed %s", bad[b], r[i].tier);
        CHECK(panel_service_ceiling_name(bad[b])[0] == '\0',
              "ceiling %d has a name", bad[b]);
    }
}

static void test_above_the_ceiling_is_locked_and_locomotion_always_is(void)
{
    for (int c = 0; c < 5; c++) {
        panel_rung_t r[PANEL_LADDER_RUNGS];
        CHECK(panel_service_ladder(c, ALL_RUN, r) == PANEL_LADDER_RUNGS,
              "rung count");
        for (int i = 0; i < PANEL_LADDER_RUNGS; i++) {
            const bool expect = (i <= c) && i < 5;
            CHECK(r[i].allowed == expect, "ceiling %d: %s allowed=%d, want %d",
                  c, r[i].tier, (int)r[i].allowed, (int)expect);
        }
        CHECK(!r[5].allowed, "LOCOMOTION allowed at ceiling %d", c);
    }
}

static void test_the_ladder_is_the_bring_up_order_and_never_bundles(void)
{
    /* The order from CLAUDE.md, written out rather than read from the table:
     * a ladder that swapped DOME and STANCE would pass every other test. */
    static const char *const order[] = {
        "READ", "LEDS", "AUDIO", "DOME", "STANCE", "LOCOMOTION",
    };
    panel_rung_t r[PANEL_LADDER_RUNGS];
    panel_service_ladder(4, ALL_RUN, r);
    for (int i = 0; i < PANEL_LADDER_RUNGS; i++) {
        CHECK(strcmp(r[i].tier, order[i]) == 0, "rung %d is %s, want %s",
              i, r[i].tier, order[i]);
    }
    /* AC6's "no run-all" is this: exactly six rungs, each one tier, in the
     * order above. A seventh rung that bundled them would fail the count. */
    CHECK(panel_service_ladder(4, ALL_RUN, r) == 6,
          "the ladder is not six single-tier rungs");
    CHECK(panel_service_ladder(0, ALL_RUN, NULL) == 0, "null ladder accepted");
}


/* ---- #168: the ceiling caps, the sequence orders ------------------------- */

static void test_a_high_ceiling_alone_does_not_open_a_rung(void)
{
    /* THE ILLEGAL CASE, and the whole point of #168: ceiling = STANCE with
     * NOTHING run. The old ladder marked all five allowed here, so an operator
     * could tap STANCE having never once moved the dome -- "bring-up order is
     * fixed" says they may not. Only READ may open: the sequence starts
     * somewhere, and READ has nothing beneath it. */
    panel_rung_t r[PANEL_LADDER_RUNGS];
    panel_service_ladder(4, 0u, r);
    CHECK(r[0].allowed, "READ shut with a STANCE ceiling and nothing run");
    for (int i = 1; i < PANEL_LADDER_RUNGS; i++) {
        CHECK(!r[i].allowed, "%s opened on a ceiling alone, nothing run",
              r[i].tier);
        const panel_rung_block_t want = (i >= 5) ? PANEL_RUNG_UNLISTED
                                                : PANEL_RUNG_SEQUENCE;
        CHECK(r[i].why == want, "%s blocked for the wrong reason (%d, want %d)",
              r[i].tier, (int)r[i].why, (int)want);
    }
}

static void test_the_sequence_opens_exactly_one_rung_at_a_time(void)
{
    /* Walk it the way an operator would, and assert the NEXT rung opens and
     * the one after does not. A gate that opened everything below the ceiling
     * once any tier ran would pass a test that only checked the next one. */
    panel_rung_t r[PANEL_LADDER_RUNGS];
    uint32_t done = 0u;
    for (int step = 0; step < 5; step++) {
        panel_service_ladder(4, done, r);
        CHECK(r[step].allowed, "step %d: %s shut when it is next",
              step, r[step].tier);
        for (int i = step + 1; i < PANEL_LADDER_RUNGS; i++)
            CHECK(!r[i].allowed, "step %d: %s opened early", step, r[i].tier);
        done |= PANEL_RUNG_BIT(step);
    }
}

static void test_a_gap_in_the_sequence_shuts_everything_above_it(void)
{
    /* READ, LEDS and DOME run; AUDIO skipped. DOME having run does not excuse
     * the gap -- STANCE stays shut, because the tier it was meant to follow
     * was never exercised. A mask test that asked "is the tier below me done"
     * instead of "is EVERY tier below me done" would pass STANCE here. */
    const uint32_t done = PANEL_RUNG_BIT(0) | PANEL_RUNG_BIT(1) |
                          PANEL_RUNG_BIT(3);
    panel_rung_t r[PANEL_LADDER_RUNGS];
    panel_service_ladder(4, done, r);
    CHECK(r[2].allowed, "AUDIO shut when READ and LEDS have run");
    CHECK(!r[3].allowed, "DOME opened across a skipped AUDIO");
    CHECK(!r[4].allowed, "STANCE opened across a skipped AUDIO");
    CHECK(r[4].why == PANEL_RUNG_SEQUENCE, "STANCE blamed the ceiling");
}

static void test_the_ceiling_is_named_before_the_sequence(void)
{
    /* Both block STANCE here: the ceiling is DOME and nothing has run. The
     * operator needs the CEILING, because no amount of walking the ladder
     * reaches a rung the gate will not admit. */
    panel_rung_t r[PANEL_LADDER_RUNGS];
    panel_service_ladder(3, 0u, r);
    CHECK(r[4].why == PANEL_RUNG_CEILING, "STANCE blamed the sequence");
    CHECK(r[1].why == PANEL_RUNG_SEQUENCE, "LEDS blamed the ceiling");
    CHECK(r[0].why == PANEL_RUNG_OPEN, "READ is not open");
}

static void test_why_and_allowed_never_disagree(void)
{
    /* Two fields describing one fact is two chances to be wrong. Sweep every
     * ceiling against every reachable `done` and pin them together. */
    for (int c = -1; c <= 6; c++) {
        for (uint32_t done = 0; done < 64u; done++) {
            panel_rung_t r[PANEL_LADDER_RUNGS];
            panel_service_ladder(c, done, r);
            for (int i = 0; i < PANEL_LADDER_RUNGS; i++)
                CHECK(r[i].allowed == (r[i].why == PANEL_RUNG_OPEN),
                      "ceiling %d done 0x%X %s: allowed=%d why=%d",
                      c, done, r[i].tier, (int)r[i].allowed, (int)r[i].why);
        }
    }
}

static void test_locomotion_is_shut_even_with_everything_run(void)
{
    /* It is not a rung of the gate at all. A sequence gate that reasoned only
     * about "is everything below me done" would open it at the top. */
    panel_rung_t r[PANEL_LADDER_RUNGS];
    panel_service_ladder(4, ALL_RUN, r);
    CHECK(!r[5].allowed, "LOCOMOTION opened with the whole ladder walked");
    /* UNLISTED, not CEILING. No ceiling admits it, so "raise the ceiling" is
     * advice that cannot work -- the wrong-lever mislead, in a test. */
    CHECK(r[5].why == PANEL_RUNG_UNLISTED, "LOCOMOTION told to raise a ceiling");
}

/* ---- #168 part 2: only a tier that moves nothing may bundle ------------- */

static void test_no_tier_that_moves_him_may_bundle(void)
{
    /* THE ILLEGAL CASE. "Each ACTUATOR test is individually opt-in, never
     * bundled" -- so every rung that makes him do something in the room fires
     * one op per tap. A STANCE tap that fires whatever STANCE contains is the
     * exact thing the rule exists to prevent, and D-010 put animations at that
     * tier because their contents cannot be inspected first. */
    static const struct { int tier; const char *name; } MOVES[] = {
        { 1, "LEDS" }, { 2, "AUDIO" }, { 3, "DOME" },
        { 4, "STANCE" }, { 5, "LOCOMOTION" },
    };
    for (unsigned i = 0; i < sizeof MOVES / sizeof MOVES[0]; i++) {
        CHECK(panel_service_tier_is_actuator(MOVES[i].tier),
              "%s is not counted as an actuator", MOVES[i].name);
        CHECK(!panel_service_tier_may_bundle(MOVES[i].tier),
              "%s is allowed to bundle", MOVES[i].name);
    }
}

static void test_read_may_bundle_because_it_moves_nothing(void)
{
    /* The one exception, and it is earned rather than special-cased: READ asks
     * his battery, his dome position and his firmware version. Three
     * questions, no actuator. */
    CHECK(!panel_service_tier_is_actuator(0), "READ counted as an actuator");
    CHECK(panel_service_tier_may_bundle(0), "READ cannot bundle three reads");
}

static void test_an_unknown_tier_is_assumed_to_move_him(void)
{
    /* A corrupt or future index that answered "no actuator" would be handed
     * the bundle, and that is the one answer that must never be a default.
     * Same shape as the ladder's out-of-range ceiling locking everything. */
    const int bad[] = { -1, 6, 7, 99, -2147483647 - 1 };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        CHECK(panel_service_tier_is_actuator(bad[i]),
              "tier %d was assumed harmless", bad[i]);
        CHECK(!panel_service_tier_may_bundle(bad[i]),
              "tier %d was granted the bundle", bad[i]);
    }
}

static void test_bundling_and_actuator_are_one_fact(void)
{
    /* Two functions describing one property is two chances to disagree. */
    for (int t = -2; t <= 8; t++)
        CHECK(panel_service_tier_may_bundle(t) ==
              !panel_service_tier_is_actuator(t),
              "tier %d: may_bundle and is_actuator disagree", t);
}

/* ---- the menu ------------------------------------------------------------- */

static void test_every_row_has_a_title_and_the_right_kind(void)
{
    static const struct { panel_svc_t s; const char *t; panel_svc_kind_t k; } want[] = {
        { PANEL_SVC_NETWORK,      "NETWORK",       PANEL_SVC_JUMP   },
        { PANEL_SVC_R2_LINK,      "R2 LINK",       PANEL_SVC_LIST   },
        { PANEL_SVC_DIAGNOSTICS,  "DIAGNOSTICS",   PANEL_SVC_LIST   },
        { PANEL_SVC_HW_TEST,      "HARDWARE TEST", PANEL_SVC_LADDER },
        { PANEL_SVC_PROVISIONING, "PROVISIONING",  PANEL_SVC_LIST   },
        { PANEL_SVC_VOICE,        "VOICE",         PANEL_SVC_NOTE   },
        { PANEL_SVC_CAMERA,       "CAMERA",        PANEL_SVC_NOTE   },
        { PANEL_SVC_ABOUT,        "ABOUT",         PANEL_SVC_LIST   },
    };
    CHECK((int)(sizeof want / sizeof want[0]) == PANEL_SVC_COUNT, "row count");
    for (unsigned i = 0; i < sizeof want / sizeof want[0]; i++) {
        CHECK(strcmp(panel_service_title(want[i].s), want[i].t) == 0,
              "row %u titled '%s'", i, panel_service_title(want[i].s));
        CHECK(panel_service_kind(want[i].s) == want[i].k, "row %s has the wrong kind", want[i].t);
    }
    CHECK(panel_service_title(PANEL_SVC_COUNT)[0] == '\0', "out of range has a title");
    CHECK(panel_service_kind((panel_svc_t)-1) == PANEL_SVC_NOTE, "out of range is listable");
}

static void test_only_lists_have_rows_and_only_notes_have_lines(void)
{
    r2_telemetry_t t = up_with_everything(1000);
    panel_svc_facts_t f = facts(&t, 2000);
    for (int s = -1; s <= PANEL_SVC_COUNT; s++) {
        panel_kv_t kv[PANEL_SVC_MAX_ROWS];
        const int n = panel_service_rows((panel_svc_t)s, &f, kv, PANEL_SVC_MAX_ROWS);
        const bool list = panel_service_kind((panel_svc_t)s) == PANEL_SVC_LIST;
        CHECK(list ? n > 0 : n == 0, "row %d gave %d rows", s, n);
        const char *a = NULL, *b = NULL;
        const bool note = panel_service_note((panel_svc_t)s, &a, &b);
        CHECK(note == (panel_service_kind((panel_svc_t)s) == PANEL_SVC_NOTE &&
                       s >= 0 && s < PANEL_SVC_COUNT),
              "row %d note=%d", s, (int)note);
        if (note) CHECK(a && b && a[0] && b[0], "row %d has an empty note", s);
    }
    CHECK(panel_service_rows(PANEL_SVC_ABOUT, NULL, NULL, 4) == 0, "null facts accepted");
}

static void test_rows_respect_max(void)
{
    r2_telemetry_t t = up_with_everything(1000);
    panel_svc_facts_t f = facts(&t, 2000);
    panel_kv_t kv[3];
    memset(kv, 0x5A, sizeof kv);
    CHECK(panel_service_rows(PANEL_SVC_R2_LINK, &f, kv, 2) == 2, "max ignored");
    CHECK(((const unsigned char *)&kv[2])[0] == 0x5A, "a row was written past max");
    CHECK(panel_service_rows(PANEL_SVC_R2_LINK, &f, kv, 0) == 0, "max 0 wrote rows");
}

static void test_every_glyph_is_in_the_font(void)
{
    /* The fonts carry 0x20-0x5F. A lowercase letter -- a git version, a link
     * name from r2_telemetry -- renders as a gap, silently. Swept across every
     * row, every title and every note, at every link state. */
    for (int l = R2_TM_DOWN; l <= R2_TM_UP; l++) {
        r2_telemetry_t t = up_with_everything(1000);
        t.link = (r2_tm_link_t)l;
        panel_svc_facts_t f = facts(&t, 5400000);
        for (int s = 0; s < PANEL_SVC_COUNT; s++) {
            const char *strs[2 + 2 * PANEL_SVC_MAX_ROWS] = { panel_service_title((panel_svc_t)s) };
            int m = 1;
            panel_kv_t kv[PANEL_SVC_MAX_ROWS];
            const int n = panel_service_rows((panel_svc_t)s, &f, kv, PANEL_SVC_MAX_ROWS);
            for (int i = 0; i < n; i++) { strs[m++] = kv[i].key; strs[m++] = kv[i].val; }
            const char *a = NULL, *b = NULL;
            if (panel_service_note((panel_svc_t)s, &a, &b)) { strs[m++] = a; strs[m++] = b; }
            for (int i = 0; i < m; i++)
                for (const char *c = strs[i]; c && *c; c++)
                    CHECK(*c >= 0x20 && *c <= 0x5F, "row %d, link %d: '%s' has 0x%02X",
                          s, l, strs[i], (unsigned char)*c);
        }
    }
}

static void test_nothing_absent_is_given_a_value(void)
{
    /* Rule 2: things this build does not have say so, in the no-claim grey. */
    r2_telemetry_t t = up_with_everything(1000);
    panel_svc_facts_t f = facts(&t, 2000);
    panel_kv_t kv[PANEL_SVC_MAX_ROWS];
    const int n = panel_service_rows(PANEL_SVC_PROVISIONING, &f, kv, PANEL_SVC_MAX_ROWS);
    const char *absent[] = { "WI-FI", "API KEY" };
    for (unsigned i = 0; i < 2; i++) {
        const panel_kv_t *r = row(kv, n, absent[i]);
        CHECK(r && strcmp(r->val, "NOT IN BUILD") == 0 && r->tone == PANEL_TONE_NONE,
              "%s reads '%s'", absent[i], r ? r->val : "(missing)");
    }
    /* The pairing rule is a glob, not a truncated name: it read "FIRST D2-"
     * before, which on the glass looked like a string that had been cut. */
    const panel_kv_t *pw = row(kv, n, "PAIRS WITH");
    CHECK(pw && strcmp(pw->val, "ANY D2-*") == 0, "PAIRS WITH reads '%s'",
          pw ? pw->val : "(missing)");

    /* A board that found no PSRAM says so rather than "0 MB". */
    f.psram_mb = 0;
    const int d = panel_service_rows(PANEL_SVC_DIAGNOSTICS, &f, kv, PANEL_SVC_MAX_ROWS);
    const panel_kv_t *p = row(kv, d, "PSRAM");
    CHECK(p && strcmp(p->val, "NONE FOUND") == 0 && p->tone == PANEL_TONE_WARN,
          "no PSRAM reads '%s'", p ? p->val : "(missing)");
    /* No firmware string is a dash, not an empty cell. */
    f.panel_fw = NULL;
    const int a = panel_service_rows(PANEL_SVC_ABOUT, &f, kv, PANEL_SVC_MAX_ROWS);
    const panel_kv_t *fw = row(kv, a, "FW");
    CHECK(fw && strcmp(fw->val, "----") == 0, "no firmware reads '%s'", fw ? fw->val : "(missing)");
}

static void test_times_format_as_whole_units(void)
{
    r2_telemetry_t t = up_with_everything(0);
    static const struct { uint32_t now; const char *up; } k[] = {
        { 0u,          "0M 00S"  },
        { 61000u,      "1M 01S"  },
        { 3599000u,    "59M 59S" },
        { 3600000u,    "1H 00M"  },
        { 3659999u,    "1H 00M"  },   /* floored: 59.999 s is not a minute */
        { 119999u,     "1M 59S"  },
        { 50400000u,   "14H 00M" },
        { 360000000u,  "4D 04H"  },
        { 0xFFFFFFFFu, "49D 17H" },
    };
    for (unsigned i = 0; i < sizeof k / sizeof k[0]; i++) {
        panel_svc_facts_t f = facts(&t, k[i].now);
        panel_kv_t kv[PANEL_SVC_MAX_ROWS];
        const int n = panel_service_rows(PANEL_SVC_ABOUT, &f, kv, PANEL_SVC_MAX_ROWS);
        const panel_kv_t *u = row(kv, n, "UPTIME");
        CHECK(u && strcmp(u->val, k[i].up) == 0, "uptime at %u ms is '%s', want '%s'",
              (unsigned)k[i].now, u ? u->val : "(missing)", k[i].up);
    }
    /* The firmware string arrives lowercase from git and leaves uppercase. */
    panel_svc_facts_t f = facts(&t, 0);
    panel_kv_t kv[PANEL_SVC_MAX_ROWS];
    const int n = panel_service_rows(PANEL_SVC_ABOUT, &f, kv, PANEL_SVC_MAX_ROWS);
    const panel_kv_t *fw = row(kv, n, "FW");
    CHECK(fw && strcmp(fw->val, "B7A3E24-DIRTY") == 0, "firmware reads '%s'", fw ? fw->val : "(missing)");
}

/* ---- review round 1 ------------------------------------------------------ */

static void test_an_unprobed_board_is_not_called_v2(void)
{
    /* THE ILLEGAL CASE: the touch controller never answered. The build is
     * for V2, and that is exactly what must NOT be repeated as a finding. */
    r2_telemetry_t t = up_with_everything(1000);
    panel_svc_facts_t f = facts(&t, 2000);
    f.touch_id = 0;
    panel_kv_t kv[PANEL_SVC_MAX_ROWS];
    int n = panel_service_rows(PANEL_SVC_DIAGNOSTICS, &f, kv, PANEL_SVC_MAX_ROWS);
    const char *unv[] = { "BOARD", "PANEL" };
    for (unsigned i = 0; i < 2; i++) {
        const panel_kv_t *r = row(kv, n, unv[i]);
        CHECK(r && strcmp(r->val, "UNVERIFIED") == 0 && r->tone == PANEL_TONE_WARN,
              "no touch answer: %s reads '%s'", unv[i], r ? r->val : "(missing)");
    }
    const panel_kv_t *tc = row(kv, n, "TOUCH");
    CHECK(tc && strcmp(tc->val, "NO ANSWER") == 0 && tc->tone == PANEL_TONE_WARN,
          "no touch answer: TOUCH reads '%s'", tc ? tc->val : "(missing)");
    n = panel_service_rows(PANEL_SVC_ABOUT, &f, kv, PANEL_SVC_MAX_ROWS);
    const panel_kv_t *b = row(kv, n, "BOARD");
    CHECK(b && strcmp(b->val, "UNVERIFIED") == 0, "ABOUT calls an unprobed board '%s'",
          b ? b->val : "(missing)");

    /* An unexpected part is named by its id, not by the one we wanted. */
    f.touch_id = 0x11;
    n = panel_service_rows(PANEL_SVC_DIAGNOSTICS, &f, kv, PANEL_SVC_MAX_ROWS);
    tc = row(kv, n, "TOUCH");
    CHECK(tc && strcmp(tc->val, "ID 0X11") == 0 && tc->tone == PANEL_TONE_WARN,
          "an unknown part reads '%s'", tc ? tc->val : "(missing)");
    f.touch_id = 0xB7;
    n = panel_service_rows(PANEL_SVC_DIAGNOSTICS, &f, kv, PANEL_SVC_MAX_ROWS);
    tc = row(kv, n, "TOUCH");
    b = row(kv, n, "BOARD");
    CHECK(tc && strcmp(tc->val, "CST820") == 0 && b && strcmp(b->val, "S3 V2") == 0,
          "a probed V2 reads '%s' / '%s'", b ? b->val : "?", tc ? tc->val : "?");
}

static void test_the_rows_are_the_same_rows_at_every_link_state(void)
{
    /* The renderer builds an interior's labels once and refreshes them in
     * place, so a row that APPEARED or VANISHED with the link would either
     * write into freed labels or leave an R2 value on screen after the drop.
     * Keys and count must not depend on the link. */
    const char *keys[PANEL_SVC_MAX_ROWS]; int n0 = -1;
    for (int l = R2_TM_DOWN; l <= R2_TM_UP; l++) {
        r2_telemetry_t t = up_with_everything(1000);
        t.link = (r2_tm_link_t)l;
        panel_svc_facts_t f = facts(&t, 5000);
        panel_kv_t kv[PANEL_SVC_MAX_ROWS];
        const int n = panel_service_rows(PANEL_SVC_R2_LINK, &f, kv, PANEL_SVC_MAX_ROWS);
        if (n0 < 0) { n0 = n; for (int i = 0; i < n; i++) keys[i] = kv[i].key; continue; }
        CHECK(n == n0, "link %d: %d rows, not %d", l, n, n0);
        for (int i = 0; i < n && i < n0; i++)
            CHECK(strcmp(kv[i].key, keys[i]) == 0, "link %d: row %d is %s, not %s",
                  l, i, kv[i].key, keys[i]);
    }
    /* And with no telemetry at all. */
    panel_svc_facts_t f = facts(NULL, 5000);
    panel_kv_t kv[PANEL_SVC_MAX_ROWS];
    const int n = panel_service_rows(PANEL_SVC_R2_LINK, &f, kv, PANEL_SVC_MAX_ROWS);
    CHECK(n == n0, "no telemetry: %d rows, not %d", n, n0);
    for (int i = 0; i < n; i++)
        CHECK(strcmp(kv[i].key, "CEILING") == 0 || kv[i].tone != PANEL_TONE_GOOD,
              "no telemetry: %s claims green", kv[i].key);
}

static void test_ages_and_a_corrupt_ceiling(void)
{
    r2_telemetry_t t = up_with_everything(0);
    static const struct { uint32_t now; const char *want; } k[] = {
        { 200u + 59999u,   "59S AGO" },
        { 200u + 60000u,   "1M AGO"  },
        { 200u + 3599999u, "59M AGO" },
        { 200u + 3600000u, "1H AGO"  },
    };
    for (unsigned i = 0; i < sizeof k / sizeof k[0]; i++) {
        panel_svc_facts_t f = facts(&t, k[i].now);
        panel_kv_t kv[PANEL_SVC_MAX_ROWS];
        const int n = panel_service_rows(PANEL_SVC_R2_LINK, &f, kv, PANEL_SVC_MAX_ROWS);
        const panel_kv_t *r = row(kv, n, "LAST READ");
        CHECK(r && strcmp(r->val, k[i].want) == 0, "age at %u: '%s', want '%s'",
              (unsigned)k[i].now, r ? r->val : "(missing)", k[i].want);
    }
    panel_svc_facts_t f = facts(&t, 1000);
    f.ceiling = 99;
    panel_kv_t kv[PANEL_SVC_MAX_ROWS];
    const int n = panel_service_rows(PANEL_SVC_R2_LINK, &f, kv, PANEL_SVC_MAX_ROWS);
    const panel_kv_t *c = row(kv, n, "CEILING");
    CHECK(c && strcmp(c->val, "----") == 0, "a corrupt ceiling reads '%s'", c ? c->val : "(missing)");
}

/* ---- the op catalogue (#168 part 2, the other half) ---------------------- */

/* A CATALOGUE FOR A TIER THAT DOES NOT EXIST, which is the only way to prove
 * the rule before it is load-bearing. The shipped table gives only READ any
 * rows, so every assertion made through panel_service_tier_ops about actuator
 * tiers is made about an empty list -- true, vacuous, and equally true with
 * the rule deleted. These three go straight into panel_service_ops_rows. */
static const panel_tier_op_t k_fake_ops[] = {
    { PANEL_OP_BATTERY, "TURN LEFT",     true,  1 },
    { PANEL_OP_HEAD,    "TURN RIGHT",    true,  1 },
    { PANEL_OP_VERSION, "WHERE ARE YOU", false, 1 },
};

/* EVERY OP IN EVERY OTHER FIXTURE SENDS EXACTLY ONE, which made the sum of
 * `sends` indistinguishable from the row count -- so `sends += ops[i].sends`
 * mutated to `sends = n_ops` SURVIVED, and so did building the label from
 * n_ops. Both tests read as though they pinned the arithmetic and neither
 * could fail. A review's mutation battery found it; this fixture is the fix.
 *
 * 2 + 3 = 5 is not 2, so the count and the sum finally disagree. */
static const panel_tier_op_t k_multi_ops[] = {
    { PANEL_OP_BATTERY, "TWO THINGS",   false, 2 },
    { PANEL_OP_HEAD,    "THREE THINGS", false, 3 },
};

/* DELETE THE may_bundle CHECK AND THIS FAILS. That is the point of it: every
 * tier from LEDS up is an actuator tier, and handed three real ops each one
 * must come back with three rows and no bundle. */
static void test_the_rule_refuses_a_bundle_for_a_tier_that_moves_him(void)
{
    for (int tier = 1; tier < PANEL_LADDER_RUNGS; tier++) {
        panel_tier_op_t out[PANEL_TIER_OPS_MAX];
        const int n = panel_service_ops_rows(tier, k_fake_ops, 3, out);
        CHECK(n == 3, "tier %d assembled %d rows from 3 ops", tier, n);
        for (int i = 0; i < n; i++)
            CHECK(out[i].op != PANEL_OP_ALL,
                  "tier %d moves him and was given a bundle row at %d", tier, i);
    }
    /* An out-of-range tier is treated as moving him, same as everywhere else. */
    panel_tier_op_t out[PANEL_TIER_OPS_MAX];
    const int n = panel_service_ops_rows(99, k_fake_ops, 3, out);
    CHECK(n == 3, "an unknown tier assembled %d rows", n);
    for (int i = 0; i < n; i++)
        CHECK(out[i].op != PANEL_OP_ALL, "an unknown tier got a bundle row");
}

/* AND IT DOES GIVE ONE TO A TIER THAT MAY BUNDLE -- the other direction, so a
 * rule that refused everything could not pass the pair.
 *
 * ITS OWN FIXTURE, because k_fake_ops is three ops that MOVE him and the ops
 * now veto a bundle regardless of what the tier table says. Written with
 * k_fake_ops this test failed the moment that veto landed, which is the veto
 * working: "READ" plus three moving ops is a list that must not bundle. */
static const panel_tier_op_t k_harmless_ops[] = {
    { PANEL_OP_BATTERY, "ASK ONE",   false, 1 },
    { PANEL_OP_HEAD,    "ASK TWO",   false, 1 },
    { PANEL_OP_VERSION, "ASK THREE", false, 1 },
};

static void test_the_rule_gives_a_bundle_to_a_tier_that_moves_nothing(void)
{
    panel_tier_op_t out[PANEL_TIER_OPS_MAX];
    const int n = panel_service_ops_rows(0, k_harmless_ops, 3, out);
    CHECK(n == 4, "READ assembled %d rows from 3 ops", n);
    if (n != 4) return;
    CHECK(out[0].op == PANEL_OP_ALL, "READ was not given a bundle row");
    CHECK(strcmp(out[0].name, "RUN ALL 3") == 0,
          "the bundle is labelled \"%s\"", out[0].name);
}

/* A LIST THAT WOULD NOT FIT IS REFUSED WHOLE. Truncating would drop the last
 * op, and a row the operator cannot see is a row they cannot tell is missing. */
static void test_a_list_too_long_to_fit_is_refused_rather_than_cut(void)
{
    panel_tier_op_t big[PANEL_TIER_OPS_MAX + 4];
    for (unsigned i = 0; i < sizeof big / sizeof big[0]; i++)
        big[i] = (panel_tier_op_t){ PANEL_OP_BATTERY, "X", false, 1 };

    panel_tier_op_t out[PANEL_TIER_OPS_MAX];
    CHECK(panel_service_ops_rows(0, big, PANEL_TIER_OPS_MAX, out) == 0,
          "a list needing a bundle row it cannot fit was not refused");
    CHECK(panel_service_ops_rows(0, big, PANEL_TIER_OPS_MAX + 4, out) == 0,
          "an oversized list was not refused");
    CHECK(panel_service_ops_rows(0, big, 0, out) == 0, "an empty list wrote rows");
    CHECK(panel_service_ops_rows(0, big, -1, out) == 0, "a negative count wrote rows");
    CHECK(panel_service_ops_rows(0, NULL, 3, out) == 0, "a NULL catalogue wrote rows");
    CHECK(panel_service_ops_rows(0, big, 3, NULL) == 0, "a NULL out was written through");
}


/* THE ILLEGAL CASE FIRST, and it is the rule itself: an actuator tier must
 * never be handed a bundle row. Walked across EVERY rung rather than the ones
 * that have a catalogue today, so the assertion still holds the day somebody
 * gives STANCE its rows -- the failure it guards against is a copy-paste of
 * READ's list onto a tier that moves him, and that copy-paste happens in a
 * tier that does not exist yet. */
static void test_an_actuator_tier_is_never_given_a_bundle_row(void)
{
    for (int tier = -3; tier < PANEL_LADDER_RUNGS + 3; tier++) {
        panel_tier_op_t ops[PANEL_TIER_OPS_MAX];
        const int n = panel_service_tier_ops(tier, ops);
        CHECK(n >= 0 && n <= PANEL_TIER_OPS_MAX, "tier %d wrote %d rows", tier, n);
        if (panel_service_tier_may_bundle(tier)) continue;
        for (int i = 0; i < n; i++)
            CHECK(ops[i].op != PANEL_OP_ALL,
                  "tier %d moves him and was given a bundle row at %d", tier, i);
    }
}

/* A TIER WITH NO CATALOGUE IS REFUSED, NOT RUN EMPTY. Every rung but READ, and
 * every index off both ends, answers 0 -- which the caller reads as "this rung
 * cannot be run". A tier that returned one unnamed row here would put a
 * tappable control on the glass for an op no code sends. */
static void test_a_tier_with_no_catalogue_answers_zero(void)
{
    panel_tier_op_t ops[PANEL_TIER_OPS_MAX];
    for (int tier = 1; tier < PANEL_LADDER_RUNGS; tier++)
        CHECK(panel_service_tier_ops(tier, ops) == 0, "tier %d has rows", tier);
    CHECK(panel_service_tier_ops(-1, ops) == 0, "tier -1 has rows");
    CHECK(panel_service_tier_ops(PANEL_LADDER_RUNGS, ops) == 0, "off the top");
    CHECK(panel_service_tier_ops(1000, ops) == 0, "far off the top");
    CHECK(panel_service_tier_ops(0, NULL) == 0, "a NULL out was written through");
}

/* READ's list: the bundle, then its three questions, in the order they go out.
 * Names are checked for CONTENT, not merely for being non-NULL -- a row whose
 * label is "" is a control the operator cannot identify, and an empty string
 * passes every NULL check. */
static void test_reads_ops_are_named_and_none_of_them_moves_him(void)
{
    panel_tier_op_t ops[PANEL_TIER_OPS_MAX];
    const int n = panel_service_tier_ops(0, ops);
    CHECK(n == 4, "READ wrote %d rows, wanted 4", n);
    if (n != 4) return;

    CHECK(ops[0].op == PANEL_OP_ALL,     "row 0 is not the bundle");
    CHECK(ops[1].op == PANEL_OP_BATTERY, "row 1 is not battery");
    CHECK(ops[2].op == PANEL_OP_HEAD,    "row 2 is not the dome read");
    CHECK(ops[3].op == PANEL_OP_VERSION, "row 3 is not the version");

    for (int i = 0; i < n; i++) {
        CHECK(ops[i].name != NULL, "row %d has no name", i);
        CHECK(ops[i].name != NULL && ops[i].name[0] != '\0',
              "row %d is labelled with an empty string", i);
        /* NOTHING AT READ MOVES HIM. That is the entire test for this tier,
         * and it is the property every rung above it leans on. */
        CHECK(ops[i].moves == false, "READ row %d claims to move him", i);
        CHECK(ops[i].sends >= 1, "row %d sends nothing", i);
    }
}

/* THE BUNDLE ROW SENDS WHAT THE SINGLES ADD UP TO. If they ever disagree, a
 * PASS on RUN ALL means fewer answers than the rows it claims to run, and the
 * sequence gate advances on a test that asked less than it said. */
static void test_the_bundle_sends_what_the_single_rows_add_up_to(void)
{
    panel_tier_op_t ops[PANEL_TIER_OPS_MAX];
    const int n = panel_service_tier_ops(0, ops);
    CHECK(n > 1 && ops[0].op == PANEL_OP_ALL, "READ has no bundle row");
    if (n <= 1 || ops[0].op != PANEL_OP_ALL) return;

    unsigned singles = 0;
    for (int i = 1; i < n; i++) {
        CHECK(ops[i].op != PANEL_OP_ALL, "a second bundle row at %d", i);
        singles += ops[i].sends;
    }
    CHECK(ops[0].sends == singles,
          "the bundle sends %u, its rows send %u", (unsigned)ops[0].sends, singles);
}

/* THE SUM, NOT THE COUNT. Two rows sending 2 and 3 must bundle to 5 and read
 * "RUN ALL 5" -- with a one-per-row fixture both of those are 2 either way,
 * which is how the arithmetic shipped untested. */
static void test_the_bundle_sums_sends_rather_than_counting_rows(void)
{
    panel_tier_op_t out[PANEL_TIER_OPS_MAX];
    const int n = panel_service_ops_rows(0, k_multi_ops, 2, out);
    CHECK(n == 3, "a 2-op list assembled %d rows", n);
    if (n != 3) return;
    CHECK(out[0].op == PANEL_OP_ALL, "row 0 is not the bundle");
    CHECK(out[0].sends == 5, "the bundle sends %u, wanted 5",
          (unsigned)out[0].sends);
    CHECK(strcmp(out[0].name, "RUN ALL 5") == 0,
          "the bundle is labelled \"%s\", wanted RUN ALL 5", out[0].name);
}

/* A SUM THAT WOULD NOT FIT A uint8_t IS REFUSED, NOT WRAPPED. A bundle row
 * claiming to send 4 while sending 260 is a control lying about what it does,
 * and the compile-time bound on the shipped table says nothing about this
 * function's arbitrary-ops contract. */
static void test_a_bundle_whose_sum_overflows_is_refused(void)
{
    const panel_tier_op_t big[] = {
        { PANEL_OP_BATTERY, "A", false, 200 },
        { PANEL_OP_HEAD,    "B", false, 200 },
    };
    panel_tier_op_t out[PANEL_TIER_OPS_MAX];
    CHECK(panel_service_ops_rows(0, big, 2, out) == 0,
          "a bundle summing to 400 was not refused");
}

/* A ROW'S NAME BELONGS TO THE ROW. The bundle's label used to point into one
 * shared static buffer, so a held row silently changed its own label when
 * somebody else asked for a different list -- demonstrated by a review probe.
 * Nothing in panel_ui held one across calls, which made it latent rather than
 * live; the comment claiming the opposite is what the next reader would have
 * believed. */
static void test_a_held_row_keeps_its_own_name(void)
{
    panel_tier_op_t a[PANEL_TIER_OPS_MAX], b[PANEL_TIER_OPS_MAX];
    const int na = panel_service_tier_ops(0, a);
    CHECK(na > 0 && a[0].op == PANEL_OP_ALL, "READ has no bundle row");
    if (na <= 0 || a[0].op != PANEL_OP_ALL) return;
    char held[PANEL_OP_NAME_LEN];
    snprintf(held, sizeof held, "%s", a[0].name);

    /* A different list, with a different sum, through the same function. */
    const int nb = panel_service_ops_rows(0, k_multi_ops, 2, b);
    CHECK(nb == 3, "the second list assembled %d rows", nb);
    CHECK(strcmp(a[0].name, held) == 0,
          "the held row's name became \"%s\" (was \"%s\")", a[0].name, held);
}

/* AN ACTUATOR TIER WITH EXACTLY PANEL_TIER_OPS_MAX OPS FITS, and used to be
 * refused whole because the capacity check reserved a bundle row for a tier
 * that can never have one. It then read as "nobody catalogued this tier",
 * which is the one thing it was not -- and the tiers likeliest to have four
 * ops are the ones that move him. */
static void test_a_full_actuator_list_fits_because_it_needs_no_bundle(void)
{
    panel_tier_op_t full[PANEL_TIER_OPS_MAX];
    for (int i = 0; i < PANEL_TIER_OPS_MAX; i++) {
        full[i] = (panel_tier_op_t){ PANEL_OP_BATTERY, "", true, 1 };
        snprintf(full[i].name, sizeof full[i].name, "OP %d", i);
    }
    panel_tier_op_t out[PANEL_TIER_OPS_MAX];
    const int n = panel_service_ops_rows(3, full, PANEL_TIER_OPS_MAX, out);
    CHECK(n == PANEL_TIER_OPS_MAX, "a full actuator list assembled %d rows", n);
    for (int i = 0; i < n; i++)
        CHECK(out[i].op != PANEL_OP_ALL, "a full actuator list got a bundle row");
}

/* AND THE OPS THEMSELVES VETO A BUNDLE. may_bundle asks a per-TIER table, and
 * this function is public and takes arbitrary ops -- handed a tier the table
 * calls harmless and an op that moves him, the bundle row would be drawn plain
 * rather than amber and would fire it. */
static void test_a_moving_op_refuses_the_bundle_whatever_the_tier_says(void)
{
    const panel_tier_op_t mixed[] = {
        { PANEL_OP_BATTERY, "WHERE ARE YOU", false, 1 },
        { PANEL_OP_HEAD,    "TURN",          true,  1 },
    };
    panel_tier_op_t out[PANEL_TIER_OPS_MAX];
    CHECK(panel_service_tier_may_bundle(0), "READ stopped being a bundling tier");
    const int n = panel_service_ops_rows(0, mixed, 2, out);
    CHECK(n == 2, "a mixed list assembled %d rows", n);
    for (int i = 0; i < n; i++)
        CHECK(out[i].op != PANEL_OP_ALL,
              "a list containing a moving op was given a bundle row");
}

/* THE LABEL NAMES THE COUNT, so a stale label is a lying control. The static
 * assert in panel_service.c pins this at compile time; this pins the rendered
 * string, which is the half an operator actually reads. */
static void test_the_bundle_label_names_the_number_it_fires(void)
{
    panel_tier_op_t ops[PANEL_TIER_OPS_MAX];
    const int n = panel_service_tier_ops(0, ops);
    CHECK(n == 4, "READ wrote %d rows", n);
    if (n != 4) return;
    char want[24];
    snprintf(want, sizeof want, "RUN ALL %u", (unsigned)ops[0].sends);
    CHECK(strcmp(ops[0].name, want) == 0,
          "the bundle says \"%s\" and fires %u", ops[0].name, (unsigned)ops[0].sends);
}

/* A TIER THAT MAY BUNDLE BUT HAS ONE OP GETS NO BUNDLE ROW -- a "RUN ALL 1"
 * beside the single op it runs is two controls for one action, and the
 * operator has to work out they are the same one.
 *
 * DRIVEN THROUGH panel_service_ops_rows, because READ is the only catalogued
 * tier and it has three: asked through panel_service_tier_ops this assertion
 * never meets a one-op list at all. A mutation battery caught it surviving
 * exactly that way -- deleting the check left the suite green. The loop below
 * is the same claim about the shipped table, kept because that is what the
 * renderer actually calls. */
static void test_a_one_op_list_is_not_given_a_bundle_of_one(void)
{
    const panel_tier_op_t one = { PANEL_OP_BATTERY, "BATTERY", false, 1 };
    panel_tier_op_t out[PANEL_TIER_OPS_MAX];
    const int n = panel_service_ops_rows(0, &one, 1, out);
    CHECK(n == 1, "a one-op list assembled %d rows", n);
    if (n >= 1)
        CHECK(out[0].op != PANEL_OP_ALL, "a single op was given a bundle row");

    for (int tier = -3; tier < PANEL_LADDER_RUNGS + 3; tier++) {
        panel_tier_op_t ops[PANEL_TIER_OPS_MAX];
        const int m = panel_service_tier_ops(tier, ops);
        if (m == 0) continue;
        CHECK(ops[0].op != PANEL_OP_ALL || m > 2,
              "tier %d bundles a single op", tier);
    }
}

/* ---- the exercised rule, once ops are tapped one at a time --------------- */

/* THE ILLEGAL CASE FIRST: one op passing must NOT exercise the tier. This is
 * the whole reason the rule needed writing -- "any op answered" is the
 * widening that per-op consent invites, and it is exactly the meaning #174
 * took away from the bit. */
static void test_one_op_passing_does_not_exercise_the_tier(void)
{
    panel_tier_op_t rows[PANEL_TIER_OPS_MAX];
    const int n = panel_service_tier_ops(0, rows);
    CHECK(n == 4, "READ wrote %d rows", n);
    if (n != 4) return;
    /* rows: [0] the bundle, [1..3] the three questions. */
    CHECK(!panel_service_tier_exercised(rows, n, 1u << 1), "battery alone exercised it");
    CHECK(!panel_service_tier_exercised(rows, n, 1u << 2), "the dome read alone exercised it");
    CHECK(!panel_service_tier_exercised(rows, n, 1u << 3), "the version alone exercised it");
    CHECK(!panel_service_tier_exercised(rows, n, (1u << 1) | (1u << 2)),
          "two of three exercised it");
    CHECK(!panel_service_tier_exercised(rows, n, 0), "nothing passing exercised it");
}

/* ALL THREE, IN ANY ORDER, DOES. The operator chose each one; that is consent
 * given three times, which is more than the bundle asked for, not less. */
static void test_every_single_op_passing_exercises_the_tier(void)
{
    panel_tier_op_t rows[PANEL_TIER_OPS_MAX];
    const int n = panel_service_tier_ops(0, rows);
    if (n != 4) { CHECK(0, "READ wrote %d rows", n); return; }
    CHECK(panel_service_tier_exercised(rows, n, (1u << 1) | (1u << 2) | (1u << 3)),
          "all three singles did not exercise the tier");
}

/* AND THE BUNDLE ALONE DOES, because its PASS already means all three were
 * answered. Checked with every single-op bit CLEAR, so it cannot be passing
 * for the other reason. */
static void test_the_bundle_alone_exercises_the_tier(void)
{
    panel_tier_op_t rows[PANEL_TIER_OPS_MAX];
    const int n = panel_service_tier_ops(0, rows);
    if (n != 4) { CHECK(0, "READ wrote %d rows", n); return; }
    CHECK(rows[0].op == PANEL_OP_ALL, "row 0 is not the bundle");
    CHECK(panel_service_tier_exercised(rows, n, 1u << 0),
          "the bundle alone did not exercise the tier");
}

/* A TIER THAT MOVES HIM HAS NO BUNDLE ROW, so its gate has to be reachable
 * through the singles alone. Driven through panel_service_ops_rows against a
 * tier with no catalogue, because that is the only way to see the shape the
 * rule will actually meet. */
static void test_an_actuator_tiers_gate_is_reachable_without_a_bundle(void)
{
    const panel_tier_op_t moving[] = {
        { PANEL_OP_BATTERY, "TURN LEFT",  true, 1 },
        { PANEL_OP_HEAD,    "TURN RIGHT", true, 1 },
    };
    panel_tier_op_t rows[PANEL_TIER_OPS_MAX];
    const int n = panel_service_ops_rows(3, moving, 2, rows);
    CHECK(n == 2, "a moving tier assembled %d rows", n);
    if (n != 2) return;
    CHECK(!panel_service_tier_exercised(rows, n, 1u << 0), "one of two exercised it");
    CHECK(!panel_service_tier_exercised(rows, n, 1u << 1), "one of two exercised it");
    CHECK(panel_service_tier_exercised(rows, n, 0x3u), "both did not exercise it");
}

/* AN EMPTY OR MALFORMED LIST IS NOT EXERCISED. A tier nobody catalogued must
 * not unlock the one above it by having nothing to do. */
static void test_a_tier_with_no_rows_is_never_exercised(void)
{
    panel_tier_op_t rows[PANEL_TIER_OPS_MAX];
    CHECK(!panel_service_tier_exercised(rows, 0, 0xFFFFFFFFu), "an empty list exercised a tier");
    CHECK(!panel_service_tier_exercised(rows, -1, 0xFFFFFFFFu), "a negative count exercised a tier");
    CHECK(!panel_service_tier_exercised(NULL, 4, 0xFFFFFFFFu), "a NULL list exercised a tier");
    CHECK(!panel_service_tier_exercised(rows, PANEL_TIER_OPS_MAX + 1, 0xFFFFFFFFu),
          "an oversized count exercised a tier");
    /* A list that is nothing but an unpassed bundle row proves nothing. */
    const panel_tier_op_t only_bundle = { PANEL_OP_ALL, "RUN ALL 2", false, 2 };
    CHECK(!panel_service_tier_exercised(&only_bundle, 1, 0),
          "a bare unpassed bundle row exercised a tier");
}

int main(void)
{
    test_one_op_passing_does_not_exercise_the_tier();
    test_every_single_op_passing_exercises_the_tier();
    test_the_bundle_alone_exercises_the_tier();
    test_an_actuator_tiers_gate_is_reachable_without_a_bundle();
    test_a_tier_with_no_rows_is_never_exercised();
    test_the_bundle_sums_sends_rather_than_counting_rows();
    test_a_bundle_whose_sum_overflows_is_refused();
    test_a_held_row_keeps_its_own_name();
    test_a_full_actuator_list_fits_because_it_needs_no_bundle();
    test_a_moving_op_refuses_the_bundle_whatever_the_tier_says();
    test_the_rule_refuses_a_bundle_for_a_tier_that_moves_him();
    test_the_rule_gives_a_bundle_to_a_tier_that_moves_nothing();
    test_a_list_too_long_to_fit_is_refused_rather_than_cut();
    test_an_actuator_tier_is_never_given_a_bundle_row();
    test_a_tier_with_no_catalogue_answers_zero();
    test_reads_ops_are_named_and_none_of_them_moves_him();
    test_the_bundle_sends_what_the_single_rows_add_up_to();
    test_the_bundle_label_names_the_number_it_fires();
    test_a_one_op_list_is_not_given_a_bundle_of_one();

    test_an_unprobed_board_is_not_called_v2();
    test_the_rows_are_the_same_rows_at_every_link_state();
    test_ages_and_a_corrupt_ceiling();
    test_a_dead_link_shows_none_of_what_he_said();
    test_accounting_survives_the_link();
    test_a_live_link_says_what_he_said();
    test_a_corrupt_ceiling_locks_everything();
    test_above_the_ceiling_is_locked_and_locomotion_always_is();
    test_the_ladder_is_the_bring_up_order_and_never_bundles();
    test_a_high_ceiling_alone_does_not_open_a_rung();
    test_the_sequence_opens_exactly_one_rung_at_a_time();
    test_a_gap_in_the_sequence_shuts_everything_above_it();
    test_the_ceiling_is_named_before_the_sequence();
    test_why_and_allowed_never_disagree();
    test_locomotion_is_shut_even_with_everything_run();
    test_no_tier_that_moves_him_may_bundle();
    test_read_may_bundle_because_it_moves_nothing();
    test_an_unknown_tier_is_assumed_to_move_him();
    test_bundling_and_actuator_are_one_fact();
    test_every_row_has_a_title_and_the_right_kind();
    test_only_lists_have_rows_and_only_notes_have_lines();
    test_rows_respect_max();
    test_every_glyph_is_in_the_font();
    test_nothing_absent_is_given_a_value();
    test_times_format_as_whole_units();
    printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
