/* What the LLM may ask of his body -- E2E v0 slice 3, step 3.4.
 *
 * The model answers a `react` tool call with two things: one of five moods,
 * and a dome angle. That is its whole reach into him. It does not choose the
 * sound -- the board picks the chirp from the mood through a committed table
 * (step 3.4a surveys it) -- so a `sound` field in a reply is ignored, and the
 * schema below does not offer one.
 *
 * THIS TIGHTENS THE MAC CONTRACT ON PURPOSE. `mac-prototype/voice/reason.py`
 * substitutes `curious` for an unknown mood and clamps an angle over 45 deg.
 * Here both are REFUSED, never repaired, so a model error cannot become a
 * move. A dome angle is 0 (no move) or 12..45 deg of travel either way. Below
 * 12 the dome silently ignores the command and still reports success (D-013),
 * and 45 is the reply ceiling (D-032).
 *
 * Provider-neutral by construction. Each vendor shapes its tool call
 * differently, so an ADAPTER turns the vendor's reply into a flat list of
 * fields, and this validator only ever sees that list. The adapters live with
 * the network code (not here); the contract they must meet is
 * voice_react_validate(), and the tests hold it without a network.
 *
 * Pure logic, no ESP dependency, host-tested. Nothing calls this yet: the
 * board's LLM client (3.4, on the network once 3.1 lands) is the first caller,
 * and the reply path (3.5, D-032) consumes its result.
 */
#ifndef VOICE_REACT_H
#define VOICE_REACT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VOICE_REACT_TIMEOUT_MS   6000u  /* the plan's 6 s; the Mac saw 14.7 s */
#define VOICE_REACT_DOME_MIN_DEG 12.0f  /* D-013: the smallest legible move */
#define VOICE_REACT_DOME_MAX_DEG 45.0f  /* D-032: the reply ceiling */

typedef enum {
    VOICE_MOOD_NONE = 0,   /* the zero value: no mood, nothing may follow */
    VOICE_MOOD_CURIOUS,
    VOICE_MOOD_HAPPY,
    VOICE_MOOD_ANNOYED,
    VOICE_MOOD_SAD,
    VOICE_MOOD_ALERT,
    VOICE_MOOD_N
} voice_mood_t;

/* One field of a tool-call reply, as an adapter extracted it. OTHER covers
 * anything that is neither a string nor a number (bool, null, object, array),
 * so a field of the wrong type is refused rather than coerced. */
typedef enum {
    VOICE_FIELD_STRING,
    VOICE_FIELD_NUMBER,
    VOICE_FIELD_OTHER,
} voice_field_type_t;

typedef struct {
    const char        *key;
    voice_field_type_t type;
    const char        *str;   /* STRING only */
    double             num;   /* NUMBER only */
} voice_field_t;

/* A validated reply. The zero value is "no reply". */
typedef struct {
    voice_mood_t mood;
    float        dome_deg;    /* 0 = no move; else 12..45 either sign, negative left */
} voice_react_t;

typedef enum {
    VOICE_REACT_OK = 0,
    VOICE_REACT_NO_MOOD,      /* no `mood` field */
    VOICE_REACT_BAD_MOOD,     /* not a string, or not one of the five, exactly */
    VOICE_REACT_NO_DOME,      /* no `dome_deg` field */
    VOICE_REACT_BAD_DOME,     /* not a finite number */
    VOICE_REACT_DOME_RANGE,   /* 0 < |deg| < 12, or |deg| > 45: refused, not clamped */
    VOICE_REACT_DUPLICATE,    /* `mood` or `dome_deg` given twice */
} voice_react_verdict_t;

/* Checks the fields and fills `out` only on OK. On any refusal `out` is left
 * as the zero value, so a caller that ignores the verdict still has no mood
 * and no move. Fields other than `mood` and `dome_deg` -- `sound` among them --
 * are ignored. */
voice_react_verdict_t voice_react_validate(const voice_field_t *fields,
                                           size_t n, voice_react_t *out);

/* ---- the provider seam --------------------------------------------------- */

typedef enum {
    VOICE_TRANSPORT_OK = 0,
    VOICE_TRANSPORT_TIMEOUT,  /* no reply within timeout_ms */
    VOICE_TRANSPORT_FAILED,   /* no Wi-Fi, no key, HTTP error, unparseable reply */
} voice_transport_t;

/* A vendor adapter. `react` asks the model about `heard`, waits at most
 * `timeout_ms`, and extracts the tool call's fields into `fields` (at most
 * `cap`; the count in `*n`). Field strings must stay valid until the next
 * call. No code outside an adapter names a vendor. */
typedef struct {
    const char *name;
    voice_transport_t (*react)(void *ctx, const char *heard, uint32_t timeout_ms,
                               voice_field_t *fields, size_t cap, size_t *n);
    void *ctx;
} voice_provider_t;

typedef enum {
    VOICE_CALL_OK = 0,        /* `out` holds a validated reply */
    VOICE_CALL_NOT_ASKED,     /* empty or missing text: the cloud was never called */
    VOICE_CALL_TIMEOUT,       /* the caller retires the exchange as TIMEOUT */
    VOICE_CALL_DOWN,          /* the face shows LLM DOWN */
    VOICE_CALL_REFUSED,       /* the model answered outside the contract; see *why */
} voice_call_t;

/* One exchange's LLM step: asks `p` with VOICE_REACT_TIMEOUT_MS and validates
 * the reply. `out` is the zero value unless the result is OK. `why` (may be
 * NULL) receives the validator's verdict when the result is REFUSED. */
voice_call_t voice_react_call(const voice_provider_t *p, const char *heard,
                              voice_react_t *out, voice_react_verdict_t *why);

/* The tool's parameter schema (JSON Schema), sent by every adapter. It offers
 * `mood` and `dome_deg` and nothing else. */
extern const char VOICE_REACT_PARAMETERS_SCHEMA[];

const char *voice_mood_name(voice_mood_t m);   /* "curious"..., "none" */
const char *voice_react_verdict_name(voice_react_verdict_t v);
const char *voice_call_name(voice_call_t c);

#endif
