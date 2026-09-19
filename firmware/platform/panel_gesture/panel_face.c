#include "panel_face.h"

#include <stddef.h>

panel_zone_t panel_face_zone(int y)
{
    if (y >= PANEL_FACE_WORD_TOP && y <= PANEL_FACE_WORD_BOTTOM) return PANEL_ZONE_WORD;
    if (y >= PANEL_FACE_LOWER_TOP) return PANEL_ZONE_LOWER;
    return PANEL_ZONE_OTHER;
}

const char *panel_face_action_name(panel_face_action_t a)
{
    switch (a) {
    case PANEL_ACT_NONE:         return "nothing";
    case PANEL_ACT_UNBLANK:      return "unblank";
    case PANEL_ACT_DISMISS_WAKE: return "dismiss wake frame";
    case PANEL_ACT_POWER:        return "wake/goodnight";
    case PANEL_ACT_TALK:         return "talk";
    case PANEL_ACT_STOP:         return "stop";
    case PANEL_ACT_PAGE:         return "page";
    case PANEL_ACT_ROW:          return "row";
    default:                     return "?";
    }
}

panel_face_action_t panel_face_resolve(const panel_face_ctx_t *c,
                                       panel_input_t in, panel_zone_t z)
{
    if (c == NULL || (int)in < 0 || in >= PANEL_IN__COUNT) return PANEL_ACT_NONE;
    if (c->page != PANEL_PAGE_STATUS && c->page != PANEL_PAGE_OTHER) return PANEL_ACT_NONE;

    /* 1-2. Something already covers the face: the landing uncovers it, and
     * nothing else in this press can reach what was underneath. */
    if (c->blanked)    return in == PANEL_IN_LAND ? PANEL_ACT_UNBLANK : PANEL_ACT_NONE;
    if (c->wake_frame) return in == PANEL_IN_LAND ? PANEL_ACT_DISMISS_WAKE : PANEL_ACT_NONE;

    /* 3. Another page: its own taps and swipes, and no holds. */
    if (c->page != PANEL_PAGE_STATUS) {
        if (in == PANEL_IN_TAP)   return PANEL_ACT_ROW;
        if (in == PANEL_IN_SWIPE) return PANEL_ACT_PAGE;
        return PANEL_ACT_NONE;
    }

    /* 4. The word's hold. GOODNIGHT wins over an exchange in progress. */
    if (z == PANEL_ZONE_WORD && in == PANEL_IN_HOLD) return PANEL_ACT_POWER;

    if (z == PANEL_ZONE_LOWER) {
        /* 5. A halt never waits: the landing itself is the STOP. */
        if (c->answering) return in == PANEL_IN_LAND ? PANEL_ACT_STOP : PANEL_ACT_NONE;
        /* 6. TALK only from rest: awake, and nothing already in flight. */
        if (in == PANEL_IN_HOLD && c->awake && !c->listening && !c->exchange)
            return PANEL_ACT_TALK;
    }

    /* 7. A swipe pages -- except under a held TALK, where the finger is
     * the microphone's switch and drifting off is not a request to leave. */
    if (in == PANEL_IN_SWIPE && !c->listening) return PANEL_ACT_PAGE;

    return PANEL_ACT_NONE;
}

void panel_face_begin(panel_face_press_t *p, int y)
{
    p->spent = false;
    p->zone = panel_face_zone(y);
}

panel_face_action_t panel_face_step(panel_face_press_t *p,
                                    const panel_face_ctx_t *c, panel_input_t in)
{
    if (p == NULL || p->spent) return PANEL_ACT_NONE;
    const panel_face_action_t a = panel_face_resolve(c, in, p->zone);
    if (a != PANEL_ACT_NONE) p->spent = true;
    return a;
}
