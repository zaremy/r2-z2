#include "voice_react.h"

#include <ctype.h>
#include <math.h>
#include <string.h>

/* The one list of mood names. The schema string below repeats them, and a
 * test holds the two together. */
static const char *const MOOD_NAMES[VOICE_MOOD_N] = {
    [VOICE_MOOD_NONE]    = "none",
    [VOICE_MOOD_CURIOUS] = "curious",
    [VOICE_MOOD_HAPPY]   = "happy",
    [VOICE_MOOD_ANNOYED] = "annoyed",
    [VOICE_MOOD_SAD]     = "sad",
    [VOICE_MOOD_ALERT]   = "alert",
};

const char VOICE_REACT_PARAMETERS_SCHEMA[] =
    "{\"type\":\"object\","
    "\"properties\":{"
      "\"mood\":{\"type\":\"string\","
        "\"enum\":[\"curious\",\"happy\",\"annoyed\",\"sad\",\"alert\"]},"
      "\"dome_deg\":{\"type\":\"number\","
        "\"description\":\"Dome turn in degrees, negative left. Must be 0 (no "
        "move) or between 12 and 45 either way; anything else is refused.\"}"
    "},"
    "\"required\":[\"mood\",\"dome_deg\"],"
    "\"additionalProperties\":false}";

/* Exact match on length, so an embedded NUL can never shorten the input. */
static int is(const char *s, size_t len, const char *lit)
{
    size_t n = strlen(lit);
    return s != NULL && len == n && memcmp(s, lit, n) == 0;
}

static voice_mood_t mood_from(const char *s, size_t len)
{
    for (int m = VOICE_MOOD_NONE + 1; m < VOICE_MOOD_N; m++)
        if (is(s, len, MOOD_NAMES[m])) return (voice_mood_t)m;
    return VOICE_MOOD_NONE;
}

voice_react_verdict_t voice_react_validate(const voice_field_t *fields,
                                           size_t n, voice_react_t *out)
{
    memset(out, 0, sizeof *out);
    const voice_field_t *mood = NULL, *dome = NULL;

    for (size_t i = 0; i < n; i++) {
        const char *k = fields[i].key;
        size_t kl = fields[i].key_len;
        if (is(k, kl, "mood")) {         /* is() refuses a NULL key */
            if (mood) return VOICE_REACT_DUPLICATE;
            mood = &fields[i];
        } else if (is(k, kl, "dome_deg")) {
            if (dome) return VOICE_REACT_DUPLICATE;
            dome = &fields[i];
        }
        /* Everything else, `sound` included, is not the model's to say. */
    }

    if (!mood) return VOICE_REACT_NO_MOOD;
    if (mood->type != VOICE_FIELD_STRING)
        return VOICE_REACT_BAD_MOOD;     /* a NULL str is refused by is() */
    voice_mood_t m = mood_from(mood->str, mood->str_len);
    if (m == VOICE_MOOD_NONE) return VOICE_REACT_BAD_MOOD;

    if (!dome) return VOICE_REACT_NO_DOME;
    if (dome->type != VOICE_FIELD_NUMBER || !isfinite(dome->num))
        return VOICE_REACT_BAD_DOME;
    double mag = fabs(dome->num);
    if (mag != 0.0 && (mag < VOICE_REACT_DOME_MIN_DEG ||
                       mag > VOICE_REACT_DOME_MAX_DEG))
        return VOICE_REACT_DOME_RANGE;

    out->mood     = m;
    out->dome_deg = (float)dome->num;
    return VOICE_REACT_OK;
}

#define MAX_FIELDS 8

voice_call_t voice_react_call(const voice_provider_t *p, const char *heard,
                              voice_react_t *out, voice_react_verdict_t *why)
{
    memset(out, 0, sizeof *out);
    if (heard == NULL) return VOICE_CALL_NOT_ASKED;
    const char *c = heard;
    while (*c && isspace((unsigned char)*c)) c++;
    if (*c == '\0') return VOICE_CALL_NOT_ASKED;   /* empty or all whitespace */

    voice_field_t fields[MAX_FIELDS];
    size_t n = 0;
    voice_transport_t t = p->react(p->ctx, heard, VOICE_REACT_TIMEOUT_MS,
                                   fields, MAX_FIELDS, &n);
    if (t == VOICE_TRANSPORT_TIMEOUT) return VOICE_CALL_TIMEOUT;
    if (t != VOICE_TRANSPORT_OK)      return VOICE_CALL_DOWN;
    if (n > MAX_FIELDS)               return VOICE_CALL_DOWN;  /* adapter overran */

    voice_react_verdict_t v = voice_react_validate(fields, n, out);
    if (why) *why = v;
    return v == VOICE_REACT_OK ? VOICE_CALL_OK : VOICE_CALL_REFUSED;
}

const char *voice_mood_name(voice_mood_t m)
{
    return ((int)m >= 0 && (int)m < VOICE_MOOD_N) ? MOOD_NAMES[m] : "?";
}

const char *voice_react_verdict_name(voice_react_verdict_t v)
{
    switch (v) {
    case VOICE_REACT_OK:         return "ok";
    case VOICE_REACT_NO_MOOD:    return "no_mood";
    case VOICE_REACT_BAD_MOOD:   return "bad_mood";
    case VOICE_REACT_NO_DOME:    return "no_dome";
    case VOICE_REACT_BAD_DOME:   return "bad_dome";
    case VOICE_REACT_DOME_RANGE: return "dome_range";
    case VOICE_REACT_DUPLICATE:  return "duplicate";
    }
    return "?";
}

const char *voice_call_name(voice_call_t c)
{
    switch (c) {
    case VOICE_CALL_OK:        return "ok";
    case VOICE_CALL_NOT_ASKED: return "not_asked";
    case VOICE_CALL_TIMEOUT:   return "timeout";
    case VOICE_CALL_DOWN:      return "down";
    case VOICE_CALL_REFUSED:   return "refused";
    }
    return "?";
}
