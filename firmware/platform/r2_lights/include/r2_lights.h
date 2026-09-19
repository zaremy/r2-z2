/* His status lights, on the board -- E2E v0 slice 2.
 *
 * A PORT, NOT A DESIGN. The light language lives in
 * `mac-prototype/r2_lights.py` (row for row from docs/behaviour-states.md),
 * and this file renders the same rows. Where the two disagree the Python is
 * right and this is a bug -- and they are compared, not trusted: the host test
 * checks every state against frames the Python itself generated
 * (`tools/gen_golden.py` -> `test/golden.h`).
 *
 * THREE THINGS THIS DOES DIFFERENTLY, and each is deliberate:
 *
 * 1. EVERY WRITE CARRIES ALL EIGHT CHANNELS, with an explicit zero for any
 *    fixture a state does not use. The Mac writes all eight when it asserts
 *    a state and only the named fixtures while animating it. One frame shape
 *    here means no write -- held back, reordered or otherwise -- can leave a
 *    fixture from an earlier state lit. Same number of writes; the payload
 *    is ten bytes instead of fewer.
 * 2. THE PLAYER SAMPLES THE CLOCK rather than walking a precomputed frame
 *    list. When a write is held back by the 120 ms spacing it sends what the
 *    state shows NOW, not a stale frame from the queue.
 * 3. `sleep` IS RENDERABLE. The Mac lists it BLOCKED because its keepalive
 *    undoes any sleep within 3 s. D-023 changed that for the board: goodnight
 *    stops the keepalive. The row is written once, just before the release --
 *    and OBSERVED 2026-09-18 not to hold after it (D-030, Consequences).
 *
 * Pure: no BLE, no clock, no ESP-IDF. The caller owns time and transport.
 */
#ifndef R2_LIGHTS_H
#define R2_LIGHTS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The Mac's row names, in the Mac's table order. */
typedef enum {
    R2L_IDLE = 0,
    R2L_WAKE,
    R2L_LISTEN,
    R2L_THINKING,
    R2L_ATTENTION,
    R2L_DANGER,
    R2L_MISHEARD,
    R2L_OFFLINE,
    R2L_WAITING,
    R2L_SLEEP,
    R2L__COUNT,
    R2L_NONE = -1,       /* no row: leave his lights alone */
} r2_lights_state_t;

/* R2 drops commands sent faster than this -- a hardware property
 * (`cmd_safe_interval`, r2_probe.py), not a tuning knob. */
#define R2_LIGHTS_MIN_INTERVAL_MS 120u

/* The wake sweep's full length: three members at 0.45 s. */
#define R2_LIGHTS_WAKE_SWEEP_MS   1350u

/* Channel order is the wire's bit order: front R,G,B, logic, back R,G,B,
 * holo. Every frame sets all eight, so the mask is always 0x00FF. */
#define R2_LIGHTS_CHANNELS 8
typedef struct { uint8_t v[R2_LIGHTS_CHANNELS]; } r2_lights_frame_t;

const char *r2_lights_name(r2_lights_state_t s);

/* What the state shows `t_ms` after it was entered, at quiet-hours scale
 * `value` in (0,1] (ignored by the states that may not dim). False for a
 * state with no row, and `out` is then untouched. */
bool r2_lights_sample(r2_lights_state_t s, uint32_t t_ms, double value,
                      r2_lights_frame_t *out);

/* ---- the player ---------------------------------------------------------- */

typedef struct {
    r2_lights_state_t state;
    uint32_t          entered_ms;
    bool              wrote_any;       /* a write has gone out since boot */
    uint32_t          last_write_ms;
    bool              owes_write;      /* entered, and nothing written yet */
    r2_lights_frame_t last;
} r2_lights_player_t;

/* A player that owes nothing and shows nothing. */
void r2_lights_player_init(r2_lights_player_t *p);

/* Enter a state. The next step owes a write even if the frame matches what
 * is already lit -- entering IS the assertion, and after a sleep or a fresh
 * connect what is lit is not ours to assume. Entering the state already
 * current is a no-op, so a caller may call this every tick. */
void r2_lights_player_enter(r2_lights_player_t *p, r2_lights_state_t s,
                            uint32_t now_ms);

/* Is a write due now? Fills `out` and returns true when one is: the state
 * was just entered, or what it shows has changed -- and in either case no
 * sooner than R2_LIGHTS_MIN_INTERVAL_MS after the previous write. A held-back
 * write is not lost; the next step sends whatever is current then.
 *
 * After a write the caller reports what happened: r2_lights_player_sent()
 * when it went out, r2_lights_player_failed() when it did not. */
bool r2_lights_player_step(r2_lights_player_t *p, uint32_t now_ms,
                           double value, r2_lights_frame_t *out);
void r2_lights_player_sent(r2_lights_player_t *p, const r2_lights_frame_t *f,
                           uint32_t now_ms);

/* A write that was attempted and refused or failed. It still costs the
 * spacing -- the bytes may have reached him anyway, and this project assumes
 * an unconfirmed command took effect -- and the frame is still owed, so it is
 * retried no sooner than R2_LIGHTS_MIN_INTERVAL_MS later rather than on
 * every tick. */
void r2_lights_player_failed(r2_lights_player_t *p, uint32_t now_ms);

/* Forget what is lit, so the next enter/step re-asserts from scratch. For a
 * link that dropped: a colour survives a drop, but a sleep in between does
 * not, and the board cannot tell which happened. */
void r2_lights_player_forget(r2_lights_player_t *p);

#ifdef __cplusplus
}
#endif
#endif /* R2_LIGHTS_H */
