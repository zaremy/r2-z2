/* Contract tests for the LLM's reach into his body (E2E v0 slice 3, step 3.4).
 *
 * The plan's gate: provider-neutral tests that an out-of-set mood or angle is
 * REFUSED, never clamped, and that a `sound` field in the reply is ignored.
 * The Mac repairs both (reason.py substitutes `curious` and clamps at 45); the
 * board must not, so each refusal is checked by its reason AND by `out`
 * staying the zero value -- a repaired reply would pass a verdict-only check
 * if the repair also returned OK.
 *
 * Illegal cases first. Legal ones last.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "voice_react.h"

static int failures = 0, checks = 0;

#define CHECK(cond, ...) do {                                \
    checks++;                                                \
    if (!(cond)) { failures++;                               \
        printf("  FAIL: "); printf(__VA_ARGS__);             \
        printf("\n        at %s:%d\n", __FILE__, __LINE__); }\
} while (0)

#define S(k, v) { (k), VOICE_FIELD_STRING, (v), 0.0 }
#define N(k, v) { (k), VOICE_FIELD_NUMBER, NULL, (v) }
#define O(k)    { (k), VOICE_FIELD_OTHER,  NULL, 0.0 }
#define LEN(a)  (sizeof (a) / sizeof (a)[0])

static int is_zero(const voice_react_t *r)
{
    return r->mood == VOICE_MOOD_NONE && r->dome_deg == 0.0f;
}

/* Validate, then check the verdict by name and that nothing leaked out. */
static void refused(const voice_field_t *f, size_t n,
                    voice_react_verdict_t want, const char *what)
{
    voice_react_t out;
    memset(&out, 0xA5, sizeof out);   /* garbage in: validate must clear it */
    voice_react_verdict_t got = voice_react_validate(f, n, &out);
    CHECK(got == want, "%s: got %s, want %s", what,
          voice_react_verdict_name(got), voice_react_verdict_name(want));
    CHECK(is_zero(&out), "%s: refused but out holds mood %d, dome %g", what,
          (int)out.mood, (double)out.dome_deg);
}

/* ---- moods: refused, never substituted --------------------------------- */

static void test_bad_moods_are_refused(void)
{
    const char *bad[] = { "Curious", "CURIOUS", " curious", "curious ",
                          "excited", "none", "", "happy!", "curio" };
    for (size_t i = 0; i < LEN(bad); i++) {
        voice_field_t f[] = { S("mood", bad[i]), N("dome_deg", 15) };
        char what[64];
        snprintf(what, sizeof what, "mood \"%s\"", bad[i]);
        refused(f, LEN(f), VOICE_REACT_BAD_MOOD, what);
    }
    voice_field_t num[]  = { N("mood", 1), N("dome_deg", 15) };
    refused(num, LEN(num), VOICE_REACT_BAD_MOOD, "numeric mood");
    voice_field_t oth[]  = { O("mood"), N("dome_deg", 15) };
    refused(oth, LEN(oth), VOICE_REACT_BAD_MOOD, "null mood");
    /* The type decides, not whether a string happens to be attached. */
    voice_field_t typed[] = { { "mood", VOICE_FIELD_NUMBER, "happy", 0 }, N("dome_deg", 15) };
    refused(typed, LEN(typed), VOICE_REACT_BAD_MOOD, "mood typed NUMBER carrying \"happy\"");
    voice_field_t nul[]  = { S("mood", NULL), N("dome_deg", 15) };
    refused(nul, LEN(nul), VOICE_REACT_BAD_MOOD, "string mood with no text");
    voice_field_t none[] = { N("dome_deg", 15) };
    refused(none, LEN(none), VOICE_REACT_NO_MOOD, "no mood");
}

/* ---- angles: refused, never clamped ------------------------------------ */

static void test_bad_angles_are_refused(void)
{
    /* The Mac clamps 60 to 45 and drops 8 to 0. The measured models returned
     * 8 despite the schema (reason.py), so this is not hypothetical. */
    const double range[] = { 8, -8, 11.99, -11.99, 0.5, 45.01, -45.01, 60, 90,
                             -180, 1e9 };
    for (size_t i = 0; i < LEN(range); i++) {
        voice_field_t f[] = { S("mood", "happy"), N("dome_deg", range[i]) };
        char what[64];
        snprintf(what, sizeof what, "dome %g", range[i]);
        refused(f, LEN(f), VOICE_REACT_DOME_RANGE, what);
    }
    const double nonfinite[] = { NAN, INFINITY, -INFINITY };
    for (size_t i = 0; i < LEN(nonfinite); i++) {
        voice_field_t f[] = { S("mood", "happy"), N("dome_deg", nonfinite[i]) };
        refused(f, LEN(f), VOICE_REACT_BAD_DOME, "non-finite dome");
    }
    voice_field_t str[] = { S("mood", "happy"), S("dome_deg", "15") };
    refused(str, LEN(str), VOICE_REACT_BAD_DOME, "dome as a string");
    voice_field_t oth[] = { S("mood", "happy"), O("dome_deg") };
    refused(oth, LEN(oth), VOICE_REACT_BAD_DOME, "dome as null");
    voice_field_t none[] = { S("mood", "happy") };
    refused(none, LEN(none), VOICE_REACT_NO_DOME, "no dome");
}

static void test_duplicates_are_refused(void)
{
    /* Which one would win is parser-dependent; refuse rather than pick. */
    voice_field_t m[] = { S("mood", "happy"), S("mood", "sad"), N("dome_deg", 15) };
    refused(m, LEN(m), VOICE_REACT_DUPLICATE, "two moods");
    voice_field_t d[] = { S("mood", "happy"), N("dome_deg", 15), N("dome_deg", 40) };
    refused(d, LEN(d), VOICE_REACT_DUPLICATE, "two domes");
}

/* ---- sound: ignored, whatever it says ---------------------------------- */

static void test_sound_is_ignored(void)
{
    voice_field_t with[][3] = {
        { S("mood", "sad"), N("dome_deg", -20), S("sound", "R2_CHATTY_1") },
        { S("mood", "sad"), N("dome_deg", -20), N("sound", 7) },
        { S("sound", "SCREAM"), S("mood", "sad"), N("dome_deg", -20) },
        { S("mood", "sad"), O("sound"), N("dome_deg", -20) },
    };
    for (size_t i = 0; i < LEN(with); i++) {
        voice_react_t out;
        voice_react_verdict_t v = voice_react_validate(with[i], 3, &out);
        CHECK(v == VOICE_REACT_OK, "sound variant %zu: %s", i,
              voice_react_verdict_name(v));
        CHECK(out.mood == VOICE_MOOD_SAD && out.dome_deg == -20.0f,
              "sound variant %zu changed the reply", i);
    }
    /* A bad reply stays bad however plausible its sound. */
    voice_field_t f[] = { S("mood", "furious"), N("dome_deg", 15), S("sound", "R2_ANNOYED") };
    refused(f, LEN(f), VOICE_REACT_BAD_MOOD, "bad mood with a sound");
    /* Unknown keys are ignored too, and a NULL key is not a crash. */
    voice_field_t g[] = { { NULL, VOICE_FIELD_STRING, "x", 0 }, S("mood", "alert"),
                          N("dome_deg", 0), S("speech", "hello") };
    voice_react_t out;
    CHECK(voice_react_validate(g, LEN(g), &out) == VOICE_REACT_OK &&
          out.mood == VOICE_MOOD_ALERT, "NULL key or extra key broke a valid reply");
}

static void test_schema_offers_only_mood_and_dome(void)
{
    const char *s = VOICE_REACT_PARAMETERS_SCHEMA;
    CHECK(strstr(s, "sound") == NULL, "the schema offers a sound");
    CHECK(strstr(s, "\"mood\"") && strstr(s, "\"dome_deg\""), "schema lacks a field");
    CHECK(strstr(s, "\"additionalProperties\":false") != NULL, "schema is open");
    /* Every mood the validator accepts is offered, and quoted exactly. */
    for (int m = VOICE_MOOD_NONE + 1; m < VOICE_MOOD_N; m++) {
        char q[32];
        snprintf(q, sizeof q, "\"%s\"", voice_mood_name((voice_mood_t)m));
        CHECK(strstr(s, q) != NULL, "schema does not offer %s", q);
    }
    CHECK(strstr(s, "\"none\"") == NULL, "schema offers the zero mood");
    CHECK(strstr(s, "12") && strstr(s, "45"), "schema does not state the bounds");
}

/* ---- the provider seam ------------------------------------------------- */

typedef struct {
    voice_transport_t   result;
    const voice_field_t *fields;
    size_t              n;
    int                 calls;
    uint32_t            timeout_seen;
} stub_t;

static voice_transport_t stub_react(void *ctx, const char *heard, uint32_t timeout_ms,
                                    voice_field_t *fields, size_t cap, size_t *n)
{
    (void)heard;
    stub_t *st = ctx;
    st->calls++;
    st->timeout_seen = timeout_ms;
    size_t k = st->n < cap ? st->n : cap;
    memcpy(fields, st->fields, k * sizeof *fields);
    *n = st->n;   /* report the true count, so an overrun is visible */
    return st->result;
}

static voice_call_t call(stub_t *st, const char *heard, voice_react_t *out,
                         voice_react_verdict_t *why)
{
    voice_provider_t p = { "stub", stub_react, st };
    memset(out, 0xA5, sizeof *out);
    return voice_react_call(&p, heard, out, why);
}

static void test_call_failure_paths(void)
{
    voice_field_t ok[] = { S("mood", "happy"), N("dome_deg", 30) };
    voice_react_t out;
    voice_react_verdict_t why = VOICE_REACT_OK;

    /* An empty transcript is MISHEARD upstream; it must never reach the cloud. */
    stub_t st = { VOICE_TRANSPORT_OK, ok, LEN(ok), 0, 0 };
    CHECK(call(&st, "", &out, &why) == VOICE_CALL_NOT_ASKED, "empty text asked");
    CHECK(call(&st, NULL, &out, &why) == VOICE_CALL_NOT_ASKED, "NULL text asked");
    CHECK(st.calls == 0, "the provider was called %d times for no text", st.calls);
    CHECK(is_zero(&out), "NOT_ASKED left a reply in out");

    stub_t to = { VOICE_TRANSPORT_TIMEOUT, ok, LEN(ok), 0, 0 };
    CHECK(call(&to, "hi", &out, &why) == VOICE_CALL_TIMEOUT, "timeout not reported");
    CHECK(is_zero(&out), "a timed-out call left a reply in out");

    stub_t dn = { VOICE_TRANSPORT_FAILED, ok, LEN(ok), 0, 0 };
    CHECK(call(&dn, "hi", &out, &why) == VOICE_CALL_DOWN, "transport failure not DOWN");
    CHECK(is_zero(&out), "a failed call left a reply in out");

    /* An adapter that claims more fields than fit is a bug, not a reply. */
    voice_field_t many[9] = { S("mood", "happy"), N("dome_deg", 30) };
    for (int i = 2; i < 9; i++) many[i] = (voice_field_t)S("pad", "x");
    stub_t ov = { VOICE_TRANSPORT_OK, many, 9, 0, 0 };
    CHECK(call(&ov, "hi", &out, &why) == VOICE_CALL_DOWN, "overrun accepted");
    CHECK(is_zero(&out), "an overrun left a reply in out");

    voice_field_t bad[] = { S("mood", "happy"), N("dome_deg", 60) };
    stub_t rf = { VOICE_TRANSPORT_OK, bad, LEN(bad), 0, 0 };
    CHECK(call(&rf, "hi", &out, &why) == VOICE_CALL_REFUSED, "60 deg not refused");
    CHECK(why == VOICE_REACT_DOME_RANGE, "refused for %s, want dome_range",
          voice_react_verdict_name(why));
    CHECK(is_zero(&out), "a refused call left a reply in out");
}

/* ---- legal cases, after the refusals ----------------------------------- */

static void test_legal_replies(void)
{
    const double legal[] = { 0, 12, -12, 30, -30, 45, -45 };
    for (size_t i = 0; i < LEN(legal); i++) {
        voice_field_t f[] = { S("mood", "curious"), N("dome_deg", legal[i]) };
        voice_react_t out;
        voice_react_verdict_t v = voice_react_validate(f, LEN(f), &out);
        CHECK(v == VOICE_REACT_OK, "dome %g: %s", legal[i], voice_react_verdict_name(v));
        CHECK(out.dome_deg == (float)legal[i], "dome %g came out as %g",
              legal[i], (double)out.dome_deg);
    }
    for (int m = VOICE_MOOD_NONE + 1; m < VOICE_MOOD_N; m++) {
        voice_field_t f[] = { S("mood", voice_mood_name((voice_mood_t)m)), N("dome_deg", 0) };
        voice_react_t out;
        CHECK(voice_react_validate(f, LEN(f), &out) == VOICE_REACT_OK &&
              out.mood == (voice_mood_t)m, "mood %s refused",
              voice_mood_name((voice_mood_t)m));
    }

    voice_field_t ok[] = { N("dome_deg", -40), S("mood", "annoyed") };  /* any order */
    stub_t st = { VOICE_TRANSPORT_OK, ok, LEN(ok), 0, 0 };
    voice_react_t out;
    voice_react_verdict_t why = VOICE_REACT_DUPLICATE;
    CHECK(call(&st, "hello", &out, &why) == VOICE_CALL_OK, "a legal call failed");
    CHECK(out.mood == VOICE_MOOD_ANNOYED && out.dome_deg == -40.0f, "legal call reply wrong");
    CHECK(st.timeout_seen == VOICE_REACT_TIMEOUT_MS && VOICE_REACT_TIMEOUT_MS == 6000u,
          "provider was given %u ms, want the plan's 6000", (unsigned)st.timeout_seen);
    CHECK(st.calls == 1, "provider called %d times", st.calls);
}

int main(void)
{
    test_bad_moods_are_refused();
    test_bad_angles_are_refused();
    test_duplicates_are_refused();
    test_sound_is_ignored();
    test_schema_offers_only_mood_and_dome();
    test_call_failure_paths();
    test_legal_replies();

    printf("%s: %d checks, %d failures\n",
           failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
