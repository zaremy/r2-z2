/* Which voice exchange is live -- E2E v0 slice 3, step 3.3b.
 *
 * An exchange runs hold -> release -> transcript -> answer, and the last two
 * arrive from the cloud seconds later (a 6 s timeout each). By then the person
 * may have said goodnight, pressed STOP, or started another hold, and R2 may
 * have been released or lost. An answer for an exchange that has already ended
 * must never enter ANSWERING, and must never reach his body. D-032's reply door
 * opens only for the live exchange.
 *
 * So every exchange gets an id when its hold starts. The id is monotonic and
 * never 0. Five events retire it: GOODNIGHT, STOP, release, link loss and a new
 * hold. The reply finishing retires it too. A result is checked against the
 * live id and the phase it belongs to. A result that fails the check is
 * DROPPED: no phase change, no id change, and panel_exchange_may_act() stays
 * false.
 *
 * The zero value is the safe value: a zeroed struct has no live exchange, and
 * every check refuses. Pure logic, no ESP dependency, host-tested.
 *
 * Nothing calls this yet. The hold (3.2), STT (3.3) and LLM (3.4) paths are
 * its callers, and D-032's reply path is the one that asks may_act.
 */
#ifndef PANEL_EXCHANGE_H
#define PANEL_EXCHANGE_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    PX_NONE = 0,      /* no live exchange */
    PX_LISTENING,     /* the hold is down, capturing */
    PX_THINKING,      /* released; waiting on the transcript, then the answer */
    PX_ANSWERING,     /* the answer was accepted; D-032's door is open */
} px_phase_t;

typedef enum {
    PX_RETIRE_NONE = 0,  /* nothing has been retired yet */
    PX_RETIRE_GOODNIGHT,
    PX_RETIRE_STOP,
    PX_RETIRE_RELEASE,
    PX_RETIRE_LINK_LOST,
    PX_RETIRE_NEW_HOLD,
    PX_RETIRE_DONE,      /* the reply finished, or the exchange ended normally */
} px_retire_t;

typedef enum {
    PX_OK = 0,
    PX_DROP_NOT_LIVE,    /* id is 0, retired, or never issued */
    PX_DROP_PHASE,       /* live id, but not the phase this result belongs to */
} px_verdict_t;

typedef struct {
    uint32_t    live;          /* 0 = none */
    uint32_t    last_issued;   /* the ids go up from here */
    px_phase_t  phase;
    bool        transcript_in; /* an answer needs a transcript before it */
    px_retire_t last_retired;  /* why the last exchange ended, for the log */
} panel_exchange_t;

void panel_exchange_reset(panel_exchange_t *px);

/* The hold starts. Any live exchange is retired as NEW_HOLD first. Returns
 * the new id, which is never 0. */
uint32_t panel_exchange_begin(panel_exchange_t *px);

/* The hold is released: LISTENING -> THINKING. */
px_verdict_t panel_exchange_released(panel_exchange_t *px, uint32_t id);

/* STT returned a transcript for `id`. Stays THINKING. */
px_verdict_t panel_exchange_transcript(panel_exchange_t *px, uint32_t id);

/* The LLM answered for `id`: THINKING -> ANSWERING. Needs a transcript first. */
px_verdict_t panel_exchange_answer(panel_exchange_t *px, uint32_t id);

/* Ends the live exchange, if there is one. No-op when there is none, and
 * last_retired is then left alone. */
void panel_exchange_retire(panel_exchange_t *px, px_retire_t why);

/* D-032: may an op for exchange `id` reach his body right now? True only for
 * the live id while ANSWERING. */
bool panel_exchange_may_act(const panel_exchange_t *px, uint32_t id);

const char *panel_exchange_verdict_name(px_verdict_t v);
const char *panel_exchange_retire_name(px_retire_t r);

#endif
