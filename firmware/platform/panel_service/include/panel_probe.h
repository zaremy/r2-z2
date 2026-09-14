/* WHAT A HARDWARE TEST SAYS WHILE IT RUNS -- #101 child 6, AC6's other half.
 *
 * The ladder lists what the gate would admit; this is what happens when the
 * operator taps a rung that it would. Pure logic, no LVGL and no radio: the
 * renderer draws the word, `main.c` sends the ops, and the decision about
 * whether a test PASSED lives here where a test can reach it.
 *
 * IT COUNTS ANSWERS, NOT SENDS. A send that the gate admitted proves the gate
 * admitted it; only an answer proves R2 heard. The caller counts the READINGS
 * that arrived since the test started (`panel_service_fresh_readings`) rather
 * than raw reply totals, which include every reply since boot.
 *
 * THAT NARROWS THE WINDOW; IT DOES NOT CLOSE IT. The panel's own periodic
 * polls -- battery every 15 s, dome every 30 s -- land in the same three
 * stamps, so a reply this test did not ask for can still count toward it
 * inside the 2 s window. Closing that would need per-request accounting the
 * telemetry layer does not carry. Stated rather than papered over: a PASS
 * means "three readings arrived while the test was running", which is weaker
 * than "R2 answered all three of these questions".
 *
 * THE ONLY RUNNABLE TIER ON THIS BUILD IS READ, and read is three questions:
 * his battery, where his dome is pointing, and his firmware version. Nothing
 * here can move him. The tiers that can are locked by the gate's ceiling,
 * which this firmware never raises (`main.c`), and the ladder refuses to offer
 * them (`panel_service_ladder`) -- two independent refusals, deliberately.
 */
#ifndef PANEL_PROBE_H
#define PANEL_PROBE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* How long a reply may take before the test is called failed. R2 answers a
 * read in well under a second on a live link -- the panel's own soak logged
 * 138/138 -- so 2 s is generous rather than tight, and a test that hangs
 * waiting is worse than one that says NO REPLY and lets you tap again. */
#define PANEL_PROBE_TIMEOUT_MS 2000u

typedef enum {
    PANEL_PROBE_IDLE = 0,   /* nothing has been asked */
    PANEL_PROBE_RUNNING,    /* asked; waiting for replies */
    PANEL_PROBE_PASS,       /* every question answered */
    PANEL_PROBE_PARTIAL,    /* some answered, then the clock ran out */
    PANEL_PROBE_NO_REPLY,   /* none answered */
    PANEL_PROBE_LINK_LOST,  /* the link went while we were waiting */
} panel_probe_state_t;

typedef struct {
    panel_probe_state_t state;
    uint32_t started_ms;
    uint8_t  expected;      /* how many answers a pass needs */
    uint8_t  got;
} panel_probe_t;

/* MAY A TAP START A TEST? The renderer's guard, lifted out of the renderer so
 * a test can reach it -- the repo's own rule is to test the guard rather than
 * the data, and a guard living inside an LVGL callback is a guard with no
 * test. Every state that means a test is already under way says no:
 *
 *   queued     -- tapped, the link task has not picked it up yet
 *   in_flight  -- picked up; its ops are going out RIGHT NOW
 *   pending    -- the ops went; the next refresh will start the clock
 *   RUNNING    -- the clock is running
 *
 * `in_flight` is the one that is easy to forget, and it is the window in which
 * a second tap used to buy six ops for one intended test: between the link
 * task taking the request and it reporting the send. On READ that is harmless
 * -- READ cannot move him.
 *
 * IT IS NOT YET ADEQUATE FOR A TIER THAT MOVES HIM, and saying otherwise here
 * would be the reassuring kind of wrong. The guard releases when the VERDICT
 * settles, which is PANEL_PROBE_TIMEOUT_MS = 2 s; D-013 measured a dome move
 * at 2.0-2.2 s regardless of distance. So the first moving tier would release
 * this guard while he is still travelling, and would report PARTIAL or NO
 * REPLY for a move that worked -- the timeout is calibrated on read latency
 * (138/138 sub-second) and is the wrong constant for every tier above READ.
 * Raising the ceiling needs a per-tier timeout, a completion signal rather
 * than a reply count, and an abort. None of those are here. */
typedef struct {
    bool queued;                /* tapped; the link task has not taken it */
    bool in_flight;             /* taken; its ops are going out RIGHT NOW */
    bool pending;               /* the ops went; the clock has not started */
    panel_probe_state_t state;  /* and what the clock says */
} panel_probe_gate_t;

/* NAMED FIELDS, NOT THREE POSITIONAL BOOLS. The caller lives in an LVGL file
 * with no host harness, so a transposed pair of arguments there compiles
 * silently and leaves every test green -- the predicate perfectly tested and
 * perfectly bypassed. Naming the fields does not make that impossible, and
 * saying so matters: `.pending = s_probe_in_flight` still compiles. What it
 * does is put the intended name beside the value at the call site, so the
 * mismatch is on the line rather than in the argument order. The untestable
 * part is smaller, not gone. */
bool panel_probe_may_start(panel_probe_gate_t g);

void panel_probe_init(panel_probe_t *p);

/* Called once the ops have actually been SENT. `expected` is how many of them
 * the gate admitted: a test that asked three questions and got two past the
 * gate needs two answers to pass, not three. Zero expected is refused -- a
 * test that asked nothing cannot pass, and reporting one that did would be
 * the rig lying. */
/* `link_up` is sampled WHERE THE OPS WERE SENT, not here. With him away every
 * send fails and `expected` is 0, which is indistinguishable at this layer
 * from a gate that refused -- and the two deserve opposite answers. */
void panel_probe_start(panel_probe_t *p, uint32_t now_ms, uint8_t expected,
                       bool link_up);

/* Feed every tick while RUNNING. `answered` is how many of the readings this
 * test asked for have arrived since it started. Returns the state, which
 * stops changing once it has settled -- the result stays on the glass until
 * the next run. */
panel_probe_state_t panel_probe_step(panel_probe_t *p, uint32_t now_ms,
                                     unsigned answered, bool link_up);

/* What the rung says now: "RUN", "...", "3/3 OK", "1/3", "NO REPLY",
 * "LINK LOST". Never empty, so a rung always has a word. */
const char *panel_probe_word(const panel_probe_t *p, char *buf, unsigned n);

/* Did it settle, and how did it go? For the renderer's colour choice. */
bool panel_probe_settled(const panel_probe_t *p);
bool panel_probe_passed(const panel_probe_t *p);

/* When the running test started, for the caller counting what has arrived
 * since. Zero when nothing is running. */
uint32_t panel_probe_started_ms(const panel_probe_t *p);

#ifdef __cplusplus
}
#endif
#endif /* PANEL_PROBE_H */
