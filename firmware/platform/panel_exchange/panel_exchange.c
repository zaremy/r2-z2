#include "panel_exchange.h"

#include <string.h>

void panel_exchange_reset(panel_exchange_t *px)
{
    memset(px, 0, sizeof *px);
}

/* The one check every result goes through, so a new result type cannot skip
 * the id test by being written without it. */
static px_verdict_t check(const panel_exchange_t *px, uint32_t id,
                          px_phase_t want)
{
    if (id == 0 || id != px->live) return PX_DROP_NOT_LIVE;
    if (px->phase != want)         return PX_DROP_PHASE;
    return PX_OK;
}

uint32_t panel_exchange_begin(panel_exchange_t *px)
{
    panel_exchange_retire(px, PX_RETIRE_NEW_HOLD);
    if (px->last_issued == UINT32_MAX) return 0;  /* exhausted: never reuse */
    uint32_t id = px->last_issued + 1;
    px->last_issued   = id;
    px->live          = id;
    px->phase         = PX_LISTENING;
    px->transcript_in = false;
    return id;
}

px_verdict_t panel_exchange_hold_released(panel_exchange_t *px, uint32_t id)
{
    px_verdict_t v = check(px, id, PX_LISTENING);
    if (v == PX_OK) px->phase = PX_THINKING;
    return v;
}

px_verdict_t panel_exchange_transcript(panel_exchange_t *px, uint32_t id,
                                       uint32_t chars)
{
    px_verdict_t v = check(px, id, PX_THINKING);
    if (v == PX_OK && px->transcript_in) v = PX_DROP_PHASE;  /* one per exchange */
    if (v == PX_OK && chars == 0) {
        panel_exchange_retire(px, PX_RETIRE_MISHEARD);
        return PX_MISHEARD;
    }
    if (v == PX_OK) px->transcript_in = true;
    return v;
}

px_verdict_t panel_exchange_answer(panel_exchange_t *px, uint32_t id)
{
    px_verdict_t v = check(px, id, PX_THINKING);
    if (v == PX_OK && !px->transcript_in) v = PX_DROP_PHASE;
    if (v == PX_OK) px->phase = PX_ANSWERING;
    return v;
}

void panel_exchange_retire(panel_exchange_t *px, px_retire_t why)
{
    if (px->live == 0) return;
    px->live          = 0;
    px->phase         = PX_NONE;
    px->last_retired  = why;
}

bool panel_exchange_may_act(const panel_exchange_t *px, uint32_t id)
{
    return check(px, id, PX_ANSWERING) == PX_OK;
}

const char *panel_exchange_verdict_name(px_verdict_t v)
{
    switch (v) {
    case PX_OK:            return "ok";
    case PX_DROP_NOT_LIVE: return "drop_not_live";
    case PX_DROP_PHASE:    return "drop_phase";
    case PX_MISHEARD:      return "misheard";
    }
    return "?";
}

const char *panel_exchange_retire_name(px_retire_t r)
{
    switch (r) {
    case PX_RETIRE_NONE:      return "none";
    case PX_RETIRE_GOODNIGHT: return "goodnight";
    case PX_RETIRE_STOP:      return "stop";
    case PX_RETIRE_R2_RELEASED: return "r2_released";
    case PX_RETIRE_LINK_LOST: return "link_lost";
    case PX_RETIRE_NEW_HOLD:  return "new_hold";
    case PX_RETIRE_HOLD_TOO_LONG: return "hold_too_long";
    case PX_RETIRE_MISHEARD:  return "misheard";
    case PX_RETIRE_TIMEOUT:   return "timeout";
    case PX_RETIRE_DONE:      return "done";
    }
    return "?";
}
