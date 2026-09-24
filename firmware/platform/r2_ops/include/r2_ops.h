/* Read-only ops — #114 slice 3.
 *
 * The first caller of both earlier slices, and it routes every send through
 * r2_gate_send(). It never touches r2_packet_encode directly: the gate is only
 * a gate if it is the only way out, and a second path to the radio would make
 * slice 2 decorative.
 *
 * All three ops sit at the READ tier and cannot move him, which is the entire
 * test for that tier. Note what that does and does not say: the READ tier means
 * OUR gate permits sending them. It says nothing about whether R2 answers --
 * the version probe may well come back bad_command_id, and that is a finding,
 * not a failure.
 *
 * CONFIDENCE, PER OP -- these are not equally known and shipping them as if
 * they were is how an INFERRED claim graduates:
 *
 *   battery   OBSERVED. Sent thousands of times; a real captured response is
 *             committed as a fixture. Payload is a big-endian uint16 in
 *             CENTIVOLTS (mac-prototype/r2_probe.py:1189 divides by 100).
 *   head      OBSERVED. Payload is a big-endian float32 in degrees
 *             (r2_probe.py:732). Accuracy measured 5/5 within 5 degrees of
 *             commanded (research/r2-capabilities.md:135).
 *   version   LIBRARY CLAIM, NEVER SENT BY US. spherov2's SystemInfo._did is
 *             17 (0x11) and get_main_app_version encodes CID 0, returning
 *             '>3H' -- three big-endian uint16 (reference/sphero-r2d2/
 *             spherov2/commands/system_info.py:41-46). But docs/research/
 *             r2-protocol.md:362 records system_info as UNTRACED, and
 *             CLAUDE.md's standing counterexample is enable_idle_animations:
 *             a capability that resolves correctly and is still refused by the
 *             firmware. Treat a version read as worth PROBING, not supported.
 *             If R2 answers bad_command_id, that is a finding, not a bug here.
 */
#ifndef R2_OPS_H
#define R2_OPS_H

#include <stdbool.h>
#include <stdint.h>

#include "r2_gate.h"
#include "r2_packet.h"
#include "voice_react.h"   /* voice_mood_t, for the reply chirp table below */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    R2_OPS_OK            =  0,
    R2_OPS_WRONG_OP      = -1,  /* response is for a different did/cid */
    R2_OPS_DEVICE_ERROR  = -2,  /* R2 answered with a non-zero error code */
    R2_OPS_BAD_LENGTH    = -3,
    R2_OPS_NULL          = -4,
} r2_ops_err_t;

/* Returned by the LED builders when the request is malformed before it ever
 * reaches the gate. Distinct from the gate's own verdicts, which are negative
 * r2_gate_verdict_t values, so a caller can tell "you asked for something
 * impossible" from "you are not allowed to ask". */
#define R2_OPS_BAD_LED_REQUEST (-100)

typedef struct { uint16_t centivolts; float volts; } r2_battery_t;
typedef struct { float degrees; }                    r2_head_t;
typedef struct { uint16_t major, minor, revision; }  r2_version_t;

/* ---- LEDs (#114 slice 5) -----------------------------------------------
 *
 * DID 0x1A / CID 0x0E, the 16-bit-mask write. Payload is
 * [mask_hi, mask_lo, values...] with one value per set bit, IN ASCENDING BIT
 * ORDER. OBSERVED: spherov2 commands/io.py:76, corroborated by
 * claude-r2d2-buddy translator.c:66 sending it on real hardware, and confirmed
 * by us -- a single write setting all eight channels returned success
 * (r2-capabilities.md:65, "AC4 confirmed"). The 8-bit variant (CID 0x1C) is
 * flagged Untested upstream and is deliberately not implemented.
 *
 * THE EIGHT CHANNELS ARE NOT EQUIVALENT, and designing as if they were has
 * already produced one wrong design in this project:
 *
 *   0,1,2  front RGB     full colour
 *   3      logic display brightness only, and effectively ON/OFF -- the two
 *          square grid panels. Do not design a fade for it.
 *   4,5,6  back RGB      full colour
 *   7      holo          brightness only, but genuinely dimmable
 *
 * And EVERY value change flickers. That is R2's LED update path, not our
 * timing (r2-capabilities.md:874-885): a 30-unit step flickers exactly as much
 * as a full swap, so there is no smooth ramp to be had at any step size. D-012
 * was written against a fade that the fixtures cannot render.
 *
 * A colour we set is STATE. It survives the link dropping and it is what the
 * household sees until he sleeps -- so leave him in a defined one.
 */
#define R2_LED_FRONT_R  0
#define R2_LED_FRONT_G  1
#define R2_LED_FRONT_B  2
#define R2_LED_LOGIC    3
#define R2_LED_BACK_R   4
#define R2_LED_BACK_G   5
#define R2_LED_BACK_B   6
#define R2_LED_HOLO     7
#define R2_LED_MAX_BIT  R2_LED_HOLO

#define R2_LED_MASK_FRONT  0x0007u
#define R2_LED_MASK_BACK   0x0070u
#define R2_LED_MASK_ALL    0x00FFu

/* Requests. Each returns the encoded length, or a negative r2_gate_verdict_t
 * if the ceiling refused it. All are READ tier, so all work at the default. */
int r2_ops_request_battery(uint8_t seq, r2_tx_fn tx, void *ctx);
int r2_ops_request_head(uint8_t seq, r2_tx_fn tx, void *ctx);
/* NOT request_ -- this one has never been answered by R2. It is named a probe
 * so no caller can read it as a supported read. */
int r2_ops_probe_version(uint8_t seq, r2_tx_fn tx, void *ctx);

/* Parsers. Each REFUSES a response that is not its own op -- otherwise a
 * battery parse would happily decode a head reading as volts. */
r2_ops_err_t r2_ops_parse_battery(const r2_response_t *r, r2_battery_t *out);
r2_ops_err_t r2_ops_parse_head(const r2_response_t *r, r2_head_t *out);
r2_ops_err_t r2_ops_parse_version(const r2_response_t *r, r2_version_t *out);

/* Set an arbitrary set of channels. values must hold exactly one byte per set
 * bit in mask, ordered by ascending bit -- a count that disagrees with the mask
 * is the one way to build a packet R2 will misread, so it is refused here
 * rather than sent. Returns the encoded length, or a negative
 * r2_gate_verdict_t: at the default READ ceiling this is R2_GATE_ABOVE_CEILING
 * and NOTHING is transmitted. Raising the ceiling is the operator's decision,
 * per the fixed bring-up order in CLAUDE.md. */
int r2_ops_set_leds(uint16_t mask, const uint8_t *values, size_t n_values,
                    uint8_t seq, r2_tx_fn tx, void *ctx);

/* A STATUS frame: all eight channels, in bit order, through the gate's
 * status path (D-030) rather than the ceiling. Refused with
 * R2_GATE_NOT_GRANTED until the operator has woken him. Every channel is
 * written, so nothing an earlier state lit can survive it. */
int r2_ops_status_leds(const uint8_t values[8], uint8_t seq,
                       r2_tx_fn tx, void *ctx);

/* Front and back RGB to one colour, in a single write. */
int r2_ops_set_rgb(uint8_t r, uint8_t g, uint8_t b,
                   uint8_t seq, r2_tx_fn tx, void *ctx);

/* Every channel to zero. Worth its own name because "leave him in a defined
 * state" is a rule here, and a teardown you have to spell out is a teardown
 * that gets skipped. */
int r2_ops_leds_off(uint8_t seq, r2_tx_fn tx, void *ctx);

/* ---- STOP ---------------------------------------------------------------
 *
 * THE SINGLE STOP IMPLEMENTATION, and there must only ever be one. The Mac
 * prototype's `stop_everything` is the source of every rule below; each was
 * learned from something going wrong.
 *
 * THREE COMMANDS, NOT ONE. Halting the animation does not halt a leg action
 * it already started -- the prototype added the legs halt after #11 with the
 * note "a stop that leaves the legs moving is not a stop, and a leg action in
 * flight is the state that put R2 on the floor". Audio is the third because a
 * droid that has stopped moving and is still shouting has not stopped.
 *
 * EVERY ONE IS ATTEMPTED, INDEPENDENTLY. The prototype's version built both
 * coroutines up front, so a failure at call time skipped the other half
 * entirely -- "the 'always attempts both' guarantee was exactly false in the
 * case it existed for". Here each send stands alone and a refusal cannot
 * short-circuit the rest.
 *
 * THE REPORT IS PER-COMMAND. The same prototype "used to print 'stop sent'
 * while discarding both Response objects -- a rejected stop and a successful
 * one were indistinguishable, on the path where nobody is watching." The
 * caller gets which of the three got out, so a partial stop can say so.
 *
 * All three are on the gate's HALT list (D-026), admitted at every ceiling.
 * A stop you have to raise a ceiling to reach is not a stop. */
typedef uint8_t (*r2_seq_fn)(void);

typedef struct {
    bool     animation;     /* the halt reached the transport */
    bool     audio;
    bool     legs;
    unsigned sent;          /* how many of the three: 3 is a full stop */
} r2_stop_report_t;

/* Fires all three. `next_seq` is called once per command -- never reuse one
 * seq for three sends, or a single reply resolves all of them. */
r2_stop_report_t r2_ops_stop_all(r2_seq_fn next_seq, r2_tx_fn tx, void *ctx);

/* True only when all three got out. Reading `sent == 3` at every call site is
 * the kind of thing that gets written `>= 2` by someone in a hurry. */
bool r2_stop_is_complete(r2_stop_report_t r);

/* ---- Reply chirp (D-032, step 3.5a) --------------------------------------
 *
 * The committed mood -> chirp id table, one id per mood, from the survivors
 * of step 3.4a's chirp survey (docs/research/r2-capabilities.md, "3.4a chirp
 * survey"). Four moods kept two candidates; this table picks one -- curious,
 * annoyed, sad and alert their first-listed survivor, happy its only one.
 * Trying the second candidate per mood is future work the survey's own vault
 * note flags, not a gap here. Returns 0 (not a real sound id) for
 * VOICE_MOOD_NONE and any value outside voice_mood_t. */
uint16_t r2_ops_chirp_id_for_mood(voice_mood_t mood);

/* Sends the one chirp a reply exchange may have, through D-032's reply door
 * (r2_gate_send_reply_audio). `may_act` must be the caller's own
 * panel_exchange_may_act(px, exchange_id) -- r2_ops has no exchange-lifecycle
 * state of its own, the same separation r2_gate itself keeps from
 * panel_exchange. Refuses (and sends nothing) for VOICE_MOOD_NONE, and for
 * every reason r2_gate_send_reply_audio refuses: no WAKE grant, `may_act`
 * false, an id outside the committed table, or a second chirp for the same
 * exchange id. Returns the encoded length, or a negative r2_gate_verdict_t. */
int r2_ops_reply_chirp(voice_mood_t mood, bool may_act, uint32_t exchange_id,
                       uint8_t seq, r2_tx_fn tx, void *ctx);

/* ---- Reply dome (D-032, step 3.5b) ---------------------------------------
 *
 * Sends the one dome move a reply exchange may have, through D-032's reply
 * door (r2_gate_send_reply_dome). A thin pass-through, unlike the chirp:
 * there is no mood-to-angle table here, because the model's own delta IS the
 * semantic content ("the model picks... an angle, and both are refused,
 * never clamped, outside their sets" -- D-032). `current_deg` must be a head
 * position read in the SAME exchange; r2_ops keeps no memory of its own and
 * trusts the caller for freshness, the same way it trusts `may_act`.
 * Refuses (and sends nothing) for every reason r2_gate_send_reply_dome
 * refuses: no WAKE grant, `may_act` false, a non-finite angle, travel
 * outside 12-45 degrees, or a second dome move for the same exchange id --
 * independently of whatever that exchange's chirp budget has done. Returns
 * the encoded length, or a negative r2_gate_verdict_t. */
int r2_ops_reply_dome(float current_deg, float delta_deg, bool may_act,
                      uint32_t exchange_id, uint8_t seq, r2_tx_fn tx, void *ctx);

/* ---- Reply composition (D-032, step 3.5c) --------------------------------
 *
 * Fires a validated reply's chirp and dome move together, through their two
 * independent doors, applying the one rule that only a caller who can see
 * BOTH doors can enforce: D-032 rule 5 -- "the chirp and the dome are
 * separate entries... when the dome entry is absent, the reply degrades to
 * the chirp. It never degrades the other way round."
 *
 * The dome move is attempted only when `reply.mood != VOICE_MOOD_NONE` (a
 * real mood to chirp for) AND `reply.dome_deg != 0` ("0 = no move",
 * voice_react.h). Gated on the MOOD, not on whether the chirp send itself
 * SUCCEEDED: both doors already enforce their own WAKE grant, `may_act`, and
 * one-per-exchange budget independently, so a chirp refused for an
 * infrastructure reason (no grant held) already refuses the dome move on the
 * identical grounds through its own gate check. What only this function can
 * prevent is a dome move riding on a reply that never had a real mood at all
 * -- exactly the "degrades the other way round" case neither door alone can
 * see.
 *
 * `current_dome_deg` must be a head position read in the SAME exchange, the
 * same freshness contract `r2_ops_reply_dome` itself carries. `next_seq` is
 * called at most once per attempted send -- never share one seq between the
 * chirp and the dome move, the same rule `r2_ops_stop_all` holds for its own
 * three sends, or a single reply resolves both.
 *
 * A field of 0 in the returned report means "not attempted" (there was no
 * reason to try); any other value is the encoded length or a negative
 * r2_gate_verdict_t from that door, exactly as `r2_ops_reply_chirp` /
 * `r2_ops_reply_dome` themselves return. */
typedef struct {
    int chirp;   /* r2_ops_reply_chirp's return, or 0 if mood was NONE */
    int dome;    /* r2_ops_reply_dome's return, or 0 if not attempted */
} r2_ops_reply_report_t;

r2_ops_reply_report_t r2_ops_reply(voice_react_t reply, bool may_act, uint32_t exchange_id,
                                   float current_dome_deg, r2_seq_fn next_seq,
                                   r2_tx_fn tx, void *ctx);

const char *r2_ops_err_name(r2_ops_err_t e);
/* R2's own error codes, r2_probe.py Response.ERRORS. */
const char *r2_ops_device_error_name(uint8_t err);

#ifdef __cplusplus
}
#endif
#endif /* R2_OPS_H */
