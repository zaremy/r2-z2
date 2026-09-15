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
#define ALL_RUN 0x3Fu

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
        CHECK(r[i].why == PANEL_RUNG_SEQUENCE || i >= 5,
              "%s blocked for the wrong reason (%d)", r[i].tier, (int)r[i].why);
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
    CHECK(r[5].why == PANEL_RUNG_CEILING, "LOCOMOTION blamed the sequence");
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

/* ---- what a hardware test may count as its own answer ------------------- */

static void test_only_readings_since_the_test_began_count(void)
{
    /* THE ILLEGAL CASE: readings that were already there. A test counting
     * those would pass on answers it never asked for -- the panel polls his
     * battery every 15 s on its own, so one is nearly always waiting. */
    r2_telemetry_t t;
    r2_telemetry_reset(&t);
    r2_telemetry_link(&t, R2_TM_UP, 1000);
    r2_telemetry_battery(&t, 442, 2000);          /* long before the test */

    CHECK(panel_service_fresh_readings(&t, 5000, 6000) == 0,
          "an older reading counted as this test's: %u",
          panel_service_fresh_readings(&t, 5000, 6000));

    /* One that arrives after it began does count. */
    r2_telemetry_dome(&t, 40.0f, 5500);
    CHECK(panel_service_fresh_readings(&t, 5000, 6000) == 1,
          "a reading during the test was not counted");
    r2_telemetry_version(&t, 7, 0, 101, 5800);
    r2_telemetry_battery(&t, 443, 5900);
    CHECK(panel_service_fresh_readings(&t, 5000, 6000) == 3, "all three not counted");

    /* A link that went takes every answer with it -- the same rule as every
     * other reading on this panel. */
    r2_telemetry_link(&t, R2_TM_SCANNING, 6000);
    CHECK(panel_service_fresh_readings(&t, 5000, 6000) == 0,
          "readings survived the link");
    CHECK(panel_service_fresh_readings(NULL, 0, 1) == 0, "null telemetry counted");
}

static void test_fresh_readings_across_the_clock_wrap(void)
{
    r2_telemetry_t t;
    r2_telemetry_reset(&t);
    const uint32_t before = 0xFFFFF000u;
    r2_telemetry_link(&t, R2_TM_UP, before);
    r2_telemetry_battery(&t, 442, before + 100u);     /* just before the wrap */
    const uint32_t after = 0x00001000u;               /* now, wrapped */
    CHECK(panel_service_fresh_readings(&t, before, after) == 1,
          "a reading across the wrap was lost");
    CHECK(panel_service_fresh_readings(&t, after - 10u, after) == 0,
          "a reading from before the window counted across the wrap");
}

int main(void)
{
    test_only_readings_since_the_test_began_count();
    test_fresh_readings_across_the_clock_wrap();
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
    test_every_row_has_a_title_and_the_right_kind();
    test_only_lists_have_rows_and_only_notes_have_lines();
    test_rows_respect_max();
    test_every_glyph_is_in_the_font();
    test_nothing_absent_is_given_a_value();
    test_times_format_as_whole_units();
    printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
