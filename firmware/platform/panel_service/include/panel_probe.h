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

void panel_probe_init(panel_probe_t *p);

/* Called once the ops have actually been SENT. `expected` is how many of them
 * the gate admitted: a test that asked three questions and got two past the
 * gate needs two answers to pass, not three. Zero expected is refused -- a
 * test that asked nothing cannot pass, and reporting one that did would be
 * the rig lying. */
void panel_probe_start(panel_probe_t *p, uint32_t now_ms, uint8_t expected);

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
