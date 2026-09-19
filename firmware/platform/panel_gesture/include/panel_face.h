/* WHAT A TOUCH ON THE FACE DOES -- E2E v0 slice 3.0b.
 *
 * One table, host-tested, for every control a finger can reach on the STATUS
 * face, in every state. Slice 3 puts four controls within a thumb of each
 * other -- the state word's hold (WAKE / GOODNIGHT), the lower face's hold
 * (TALK), the face STOP, and the page swipe -- on top of two that already
 * swallow a touch whole (the wake frame, and the blanked released face). A
 * rule that lives in ui_task's control flow is a rule nobody can enumerate;
 * this is where it can be.
 *
 * THE INVARIANT, and what the tests prove: ONE PRESS, AT MOST ONE ACTION.
 * A press passes through several inputs -- it lands, it may complete a hold,
 * it lifts as a tap or a swipe -- and the first input that yields an action
 * spends the press, so nothing later in the same press can do anything else.
 *
 * Precedence, highest first (panel spec §9, vault Prototypes/README.md):
 *   1. a blanked face: the landing lights it, and that is all (§10)
 *   2. the wake frame: the landing dismisses it, and that is all (D-017)
 *   3. off the STATUS page: taps and swipes are the page's own
 *   4. the state word, held: WAKE / GOODNIGHT -- and GOODNIGHT wins over an
 *      exchange in progress (§9)
 *   5. the lower face while ANSWERING: STOP on the landing -- a halt never
 *      waits for a hold (D-026, D-028's face clause)
 *   6. the lower face, held, while awake and idle: TALK
 *   7. a swipe: the next page -- except while TALK is being held
 *
 * Pure: no LVGL, no touch driver. The caller classifies the press and says
 * what the panel is doing; this says what the press means.
 */
#ifndef PANEL_FACE_H
#define PANEL_FACE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Where on the face a press LANDED, from its y in panel pixels. The word band
 * is slice 1's hold target; the rule under it is at y=135, and a press landing
 * anywhere in 131-139 belongs to neither hold, so the two cannot share a
 * pixel. */
#define PANEL_FACE_WORD_TOP     40
#define PANEL_FACE_WORD_BOTTOM 130
#define PANEL_FACE_LOWER_TOP   140

typedef enum {
    PANEL_ZONE_OTHER = 0,      /* the chrome above, and the rule's gap */
    PANEL_ZONE_WORD,
    PANEL_ZONE_LOWER,
} panel_zone_t;

panel_zone_t panel_face_zone(int y);

/* One thing that happened to a press. */
typedef enum {
    PANEL_IN_LAND = 0,         /* the finger touched down */
    PANEL_IN_HOLD,             /* it has been held PANEL_HOLD_MS in place */
    PANEL_IN_TAP,              /* it lifted as a tap */
    PANEL_IN_SWIPE,            /* it lifted as a swipe */
    PANEL_IN__COUNT,
} panel_input_t;

/* Which page the finger is on. ZERO IS "UNKNOWN", not a page: a context
 * nobody filled in must not be mistaken for SERVICE and hand its taps to a
 * row (the first test run caught exactly that). */
typedef enum {
    PANEL_PAGE_UNKNOWN = 0,
    PANEL_PAGE_STATUS,
    PANEL_PAGE_OTHER,          /* SERVICE: taps and swipes of its own */
} panel_face_page_t;

/* What the panel is doing when the input arrives. The ZERO VALUE IS THE SAFE
 * VALUE: an all-false context is on no known page, asleep and released, and
 * resolves every input to nothing. */
typedef struct {
    panel_face_page_t page;
    bool blanked;              /* the released face's burn-in blank (§10) */
    bool wake_frame;           /* D-017's wake frame is up */
    bool awake;                /* wanted: the operator woke him */
    bool listening;            /* TALK is being held */
    bool exchange;             /* thinking or answering: a reply is coming */
    bool answering;            /* a reply can reach his body right now */
} panel_face_ctx_t;

typedef enum {
    PANEL_ACT_NONE = 0,
    PANEL_ACT_UNBLANK,
    PANEL_ACT_DISMISS_WAKE,
    PANEL_ACT_POWER,           /* WAKE or GOODNIGHT; link_task resolves which */
    PANEL_ACT_TALK,
    PANEL_ACT_STOP,
    PANEL_ACT_PAGE,
    PANEL_ACT_ROW,             /* a tap handed to the page's own rows */
    PANEL_ACT__COUNT,
} panel_face_action_t;

const char *panel_face_action_name(panel_face_action_t a);

/* The table itself: what one input means in one context and zone, ignoring
 * whether the press has already done something. */
panel_face_action_t panel_face_resolve(const panel_face_ctx_t *c,
                                       panel_input_t in, panel_zone_t z);

/* A press in progress. Begin it on PANEL_IN_LAND; step every input through
 * it. The first input that yields an action spends it, and every later input
 * in the same press yields NONE -- the caller must void the rest of the
 * gesture when it gets anything but NONE. */
typedef struct {
    bool         spent;
    panel_zone_t zone;          /* fixed where the press landed */
} panel_face_press_t;

void panel_face_begin(panel_face_press_t *p, int y);
panel_face_action_t panel_face_step(panel_face_press_t *p,
                                    const panel_face_ctx_t *c, panel_input_t in);

#ifdef __cplusplus
}
#endif
#endif /* PANEL_FACE_H */
