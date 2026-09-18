/* WHAT A HARDWARE TEST SAYS WHILE IT RUNS -- #101 child 6, AC6's other half.
 *
 * The ladder lists what the gate would admit; this is what happens when the
 * operator taps a rung that it would. Pure logic, no LVGL and no radio: the
 * renderer draws the word, `main.c` sends the ops, and the decision about
 * whether a test PASSED lives here where a test can reach it.
 *
 * IT COUNTS ANSWERS, NOT SENDS. A send that the gate admitted proves the gate
 * admitted it; only an answer proves R2 heard. The caller counts the replies
 * that struck one of THIS test's own requests (`panel_ledger_t`) rather than
 * raw reply totals, which include every reply since boot.
 *
 * AND IT NOW COUNTS THE RIGHT ANSWERS. It used to count READINGS that
 * arrived in the window, which the panel's own periodic polls -- battery
 * every 15 s, dome every 30 s -- also land in. So a PASS meant "three
 * readings arrived while the test was running", not "R2 answered all three
 * of these questions", and for a tier that MOVES him that gap is the whole
 * ballgame: the sequence gate (D-025) could advance to STANCE on a background
 * battery poll, with the dome never having turned.
 *
 * Every request carries a sequence number and every reply carries it back.
 * `panel_ledger_t` writes down the seqs a test sent and strikes them off as
 * their own replies arrive, so a poll's reply matches nothing and counts for
 * nothing. A PASS now means what it sounds like.
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

/* D-013: a dome move takes 2.0-2.2 s whatever the distance, so the window has
 * to clear the move itself plus the reply. Not a guess -- the move is measured
 * and this is it, rounded up with room for the answer to come back. */
#define PANEL_PROBE_DOME_TIMEOUT_MS 4000u

/* HOW LONG HE IS ACTUALLY MOVING, which is not how long the verdict takes.
 * D-013 clocked four dome moves and the longest was 2.19 s (8.35 deg took
 * 2.19 s, 22.61 deg took 2.07 s -- fixed duration, not a slew rate), so 2200
 * is the measured upper bound rounded up to the next 100 ms, not a guess.
 *
 * This is a SEPARATE number from the timeout above and must stay separate.
 * The timeout is "how long may the answer take"; this is "how long until he
 * has stopped". They are close today only by coincidence of one tier. */
#define PANEL_PROBE_DOME_MOVE_MS 2200u

/* HOW FAR BEHIND A SEND'S STAMP THE GUARD'S CLOCK CAN LEGITIMATELY BE. The
 * stamp is taken on link_task and the guard reads ui_task's last tick, so the
 * gap is one refresh -- tens of ms. Anything within this reads as "still
 * moving"; anything further is elapsed time that has wrapped, not a clock
 * behind. Generous on purpose: the cost of the margin is one false hold of at
 * most this long, once per ~49.7-day clock wrap. */
#define PANEL_MOTION_SKEW_MS 10000u

typedef enum {
    PANEL_PROBE_IDLE = 0,   /* nothing has been asked */
    PANEL_PROBE_RUNNING,    /* asked; waiting for replies */
    PANEL_PROBE_PASS,       /* every question answered */
    PANEL_PROBE_PARTIAL,    /* some answered, then the clock ran out */
    PANEL_PROBE_NO_REPLY,   /* none answered */
    PANEL_PROBE_LINK_LOST,  /* he was away when we asked, or went while we
                             * waited -- either way the question never
                             * reached him, and the silence is ours */
    PANEL_PROBE_STOPPED,    /* the operator hit STOP before it settled. Never
                             * a pass: whatever came back afterwards answers a
                             * test that was halted, not one that ran */
} panel_probe_state_t;

typedef struct {
    panel_probe_state_t state;
    uint32_t started_ms;
    uint8_t  expected;      /* how many answers a pass needs */
    uint8_t  got;
    int      tier;          /* which rung, for its own timeout */
    bool     completion_armed;  /* the notify went out on this same path */
    bool     completed;         /* and the completion event came back */
} panel_probe_t;

/* WHAT THIS TEST ASKED, AND WHAT HAS COME BACK.
 *
 * A reply is matched to a request by its sequence number, so the panel's own
 * battery and dome polls -- which are asked on their own seqs -- cannot
 * satisfy a test that did not ask them. That is the difference between "three
 * readings arrived" and "R2 answered these three questions", and it is the
 * difference the sequence gate keys on.
 *
 * SEQ IS ONE BYTE AND WRAPS. R2's counter walks all 256 every ~12.8 min
 * (r2_packet.h), so a stale reply could in principle carry a seq this test is
 * waiting on. It would take 256 sends inside one test's window to alias, and
 * a window is seconds -- but the ledger is still written to strike a seq only
 * ONCE, so a duplicate can at worst fail to count, never double-count. */
#define PANEL_LEDGER_MAX 8

typedef struct {
    uint8_t seq[PANEL_LEDGER_MAX];
    bool    open[PANEL_LEDGER_MAX];   /* not yet answered */
    uint8_t asked;
    uint8_t answered;
} panel_ledger_t;

void panel_ledger_reset(panel_ledger_t *l);

/* Record a request this test sent. Refuses a seq already outstanding: one
 * reply must never strike two requests, which would let a test of three
 * questions pass on two answers. Refuses once full. */
bool panel_ledger_add(panel_ledger_t *l, uint8_t seq);

/* A reply arrived. True only if it struck an OPEN request of this test's --
 * a background poll, a keepalive, or a second copy of an answer already
 * counted all return false and change nothing. */
bool panel_ledger_note(panel_ledger_t *l, uint8_t seq);

unsigned panel_ledger_asked(const panel_ledger_t *l);
unsigned panel_ledger_answered(const panel_ledger_t *l);

/* HOW LONG THAT TIER MAY TAKE, or 0 for "nobody has measured this".
 *
 * READ is 2 s because it was measured: 138 of 138 replies sub-second on a live
 * link. DOME is the only other tier with a number behind it -- D-013 clocked a
 * move at 2.0-2.2 s REGARDLESS of distance, which is why the old single 2 s
 * constant would have called a working move PARTIAL.
 *
 * LEDS, AUDIO and STANCE return 0, and a 0 is REFUSED rather than defaulted.
 * Nobody has timed them. A guessed constant here reads exactly like a measured
 * one six months from now, and the repo's rule is that an INFERRED claim must
 * not silently graduate to OBSERVED. The commit that raises the ceiling to one
 * of those tiers has to go and measure it first -- which is the correct amount
 * of friction for a rung that moves him.
 *
 * panel_probe_start REFUSES a tier whose window is 0: it settles NO REPLY
 * without asking anything, rather than running on a number nobody measured. */
uint32_t panel_probe_timeout_ms(int tier);

/* HOW LONG THAT TIER MOVES HIM, or 0 for a tier that cannot move him at all.
 *
 * READ is 0 because reading cannot move him -- structural, not measured. DOME
 * is PANEL_PROBE_DOME_MOVE_MS because D-013 measured it. Everything else is 0.
 *
 * SO 0 MEANS TWO THINGS -- "cannot move him" and "nobody timed it" -- and only
 * the first is safe to release on. panel_probe_needs_completion tells them
 * apart, and panel_motion_settled holds the guard for a moving tier whose
 * move is 0. That hold is the safety; a 0 window is NOT, because
 * panel_probe_start refuses the VERDICT after link_task has already sent the
 * ops. test_panel_probe still asserts that every tier with a window and a
 * move has a measured duration, so a tier made runnable gets timed rather
 * than locking the panel until reboot. */
uint32_t panel_probe_move_ms(int tier);

/* DOES A PASS ON THIS TIER NEED A COMPLETION EVENT? True for every tier that
 * moves him. A reply count says the op was heard; only a completion says he
 * finished doing it, and for a tier that moves him those are different
 * questions with different answers. */
bool panel_probe_needs_completion(int tier);

/* WHAT WENT OUT THAT CAN MOVE HIM, AND WHEN -- kept apart from the verdict.
 *
 * #187 keyed the guard on the PROBE, and the probe is the display's: leaving
 * the op list calls panel_probe_init, and a send still in flight when the
 * operator backs out is disowned by generation and never starts a probe at
 * all. Either way the probe read IDLE, IDLE read "nothing was started", and
 * the guard opened -- so backing out and straight back in bought a second
 * dome move into the first. Disowning the display had disowned the command.
 *
 * This record is written where the ops actually went, whatever the display
 * did meanwhile, and nothing that closes a screen touches it. The zero value
 * means "nothing has been sent", which is true at boot and only then. */
typedef struct {
    bool     sent;      /* something went out on a tier that can move him */
    int      tier;
    uint32_t sent_ms;   /* stamped AFTER the sends, so the hold errs long */
} panel_motion_t;

/* The ops for `tier` went out and `sent` of them reached the transport.
 *
 * A tier that cannot move him leaves the record alone: a READ sent mid-move
 * would otherwise restamp it and release a hold the dome is still owed. A
 * tier that moves him with no measured move (STANCE today) is recorded and
 * holds the guard shut until reboot -- see panel_motion_settled. A send of 0 leaves it alone too, for a different reason --
 * nothing went out, so nothing is moving, and a DOME tap with the link down
 * must not lock every row out for a move that never happened. */
void panel_motion_note_sent(panel_motion_t *m, int tier, unsigned sent,
                            uint32_t sent_ms);

/* HAS HE STOPPED? True once the tier's measured move duration has elapsed
 * since its ops went out, and always true when nothing that moves him has.
 * This is the guard's question -- NOT `settled`, which asks about the verdict
 * and answers it at 2 s while a 2.19 s move is still running. A NULL record
 * is "still moving": a caller that lost track of the command must refuse. */
bool panel_motion_settled(const panel_motion_t *m, uint32_t now_ms);

/* THE COMPLETION CHANNEL IS ARMED, and the caller is saying so on the same
 * path that enabled it. `leg_action_complete` does not fire unless
 * `notify --params '{"leg":true}'` went out; a session that forgets still
 * gets `animation_complete`, still measures durations, and sees ZERO leg
 * events -- a wrong answer in the safe-looking direction (CLAUDE.md). So a
 * tier that needs a completion is REFUSED unless this was called with true.
 *
 * panel_probe_init clears it. Omission therefore refuses rather than runs,
 * which is the only direction a default may fail in here. */
void panel_probe_arm_completion(panel_probe_t *p, bool armed);

/* THE COMPLETION EVENT ARRIVED. Idempotent; ignored when nothing is running. */
void panel_probe_note_complete(panel_probe_t *p);

/* THE OPERATOR HIT STOP. A test that has not settled -- running, or tapped and
 * not yet started -- becomes STOPPED, and STOPPED never becomes a pass: an
 * answer or a completion arriving after the halt is about a test that was
 * interrupted, and scoring it would award a PASS to the tap the operator
 * tried to take back. A test that had already settled keeps its verdict; the
 * STOP came after it and says nothing about it.
 *
 * THIS DOES NOT RELEASE THE GUARD, and must not. The three halts are
 * animation, audio and legs (D-026); none of them is a dome halt, so a dome
 * move already under way runs its measured course regardless. The guard's
 * hold is panel_motion_t's, which a STOP does not touch. */
void panel_probe_abort(panel_probe_t *p);

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
 * AND IT NOW HOLDS THROUGH THE MOVE, which is the fifth state and the one
 * this guard was missing (#184). The verdict settling is not him stopping:
 * the verdict settled at PANEL_PROBE_TIMEOUT_MS = 2 s while D-013's measured
 * dome move ran to 2.19 s, so the guard released mid-travel and a second tap
 * fired a move into the first. `motion_settled` is now part of the decision,
 * and it is keyed on the tier's own MEASURED move duration
 * (panel_probe_move_ms), not on the read-latency constant.
 *
 * `motion_settled` comes from panel_motion_settled, NOT from the probe: the
 * probe belongs to the display and is reset whenever a screen closes, and a
 * guard fed from it opened the moment the operator backed out mid-move. */
typedef struct {
    bool queued;                /* tapped; the link task has not taken it */
    bool in_flight;             /* taken; its ops are going out RIGHT NOW */
    bool pending;               /* the ops went; the clock has not started */
    panel_probe_state_t state;  /* and what the clock says */
    /* HAS HE STOPPED MOVING? Phrased as the SAFE condition, not the unsafe
     * one, because C zero-fills a designated initializer: a caller that
     * forgets this field gets false, which reads as "still moving" and
     * REFUSES the tap. The unsafe default was the whole defect -- a `.moving`
     * field omitted would have read as "not moving" and released the guard
     * mid-travel. Fail closed by construction, not by remembering. */
    bool motion_settled;
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
/* `tier` picks the window. A tier with no measured window is REFUSED here
 * rather than given a default -- see panel_probe_timeout_ms. */
void panel_probe_start(panel_probe_t *p, uint32_t now_ms, uint8_t expected,
                       bool link_up, int tier);

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


#ifdef __cplusplus
}
#endif
#endif /* PANEL_PROBE_H */
