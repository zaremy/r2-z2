/* The permission ceiling — #114 slice 2.
 *
 * WHY THIS EXISTS, AND WHY IT COMES BEFORE ANY OP THAT CAN MOVE HIM.
 *
 * The Mac daemon has a ceiling: `r2_probe.py --allow <tier>` fixes what a
 * session may emit, and CLAUDE.md sets the bring-up order — read-only, LEDs,
 * audio, small dome, stance, locomotion — with each actuator individually
 * opt-in and never bundled. The firmware has had no equivalent.
 *
 * An ESP32 that can send arbitrary Sphero packets, bolted to the droid, is
 * strictly worse than the Mac path: it is always on, it is unattended, and
 * nobody has to type a flag. So the gate lands BEFORE the first op that can
 * move him exists, rather than after.
 *
 * THREE PROPERTIES THAT MATTER MORE THAN THE TABLE:
 *
 * 1. It is an ALLOWLIST. An op that is not listed is refused. A denylist would
 *    permit every command nobody thought about, which for a device with legs is
 *    the wrong default.
 * 2. FORBIDDEN ops are named explicitly and checked FIRST, so they stay refused
 *    even if someone later adds them to the allowlist by mistake. A positive
 *    assertion about a known list beats the absence of a match.
 * 3. The check is on the SEND PATH, not at construction. A guard where the
 *    packet is built is a guard on the path people happen to use; a guard where
 *    the bytes leave is a guard on the path that has the effect.
 */
#ifndef R2_GATE_H
#define R2_GATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Low to high. Mirrors r2_behavior.TIERS ("read","leds","audio","dome","stance").
 * Locomotion is deliberately NOT a rung here: it is out of scope for #114 and
 * has no allowlist entry, so it is refused as unlisted. */
typedef enum {
    R2_TIER_READ = 0,
    R2_TIER_LEDS,
    R2_TIER_AUDIO,
    R2_TIER_DOME,
    R2_TIER_STANCE,
    R2_TIER__COUNT
} r2_tier_t;

typedef enum {
    R2_GATE_ALLOW           =  0,
    R2_GATE_ABOVE_CEILING   = -1,  /* known op, ceiling too low */
    R2_GATE_NOT_ALLOWLISTED = -2,  /* unknown op: refused by default */
    R2_GATE_FORBIDDEN       = -3,  /* never permitted at any ceiling */
    R2_GATE_NO_TX           = -4,  /* no transmit function supplied */
    R2_GATE_ENCODE_FAILED   = -5,
    R2_GATE_NOT_GRANTED     = -6,  /* status path: nobody has woken him */
    R2_GATE_NOT_STATUS      = -7,  /* status path: that op is not a light */
} r2_gate_verdict_t;

/* Transmit hook. Returns >=0 on success. Kept as a callback so the gate has no
 * BLE dependency and is testable on the host. */
typedef int (*r2_tx_fn)(const uint8_t *frame, size_t len, void *ctx);

/* Defaults to R2_TIER_READ — the lowest rung — so a firmware that forgets to
 * set a ceiling can still only read. */
void      r2_gate_set_ceiling(r2_tier_t ceiling);
r2_tier_t r2_gate_get_ceiling(void);

/* Verdict for one op, without sending. Exposed for diagnostics and tests. */
/* THE PAYLOAD IS PART OF THE VERDICT. `perform_leg_action` is forbidden as a
 * motion and admitted as a halt, and the only thing that tells those apart is
 * the byte it carries -- so a check that cannot see the payload cannot answer
 * correctly. There is deliberately no did+cid-only variant: two checks that can
 * disagree about one op is how a gate gets bypassed by the more convenient
 * one. Pass NULL/0 for an op that carries nothing. */
r2_gate_verdict_t r2_gate_check(uint8_t did, uint8_t cid,
                                const uint8_t *data, size_t data_len);

/* THE SANCTIONED WAY OUT, and one of exactly two -- the other is
 * r2_gate_send_status below. Checks, encodes, transmits. Returns the encoded
 * length on success, or a negative r2_gate_verdict_t. */
int r2_gate_send(uint8_t did, uint8_t cid, uint8_t seq,
                 const uint8_t *data, size_t data_len,
                 r2_tx_fn tx, void *ctx);

/* THE STATUS PATH (D-030, E2E v0 slice 2).
 *
 * His status lights have to change on their own -- nobody taps "idle" -- and
 * D-029's consent is a tap per named op. So this is a second exit, and it is
 * as narrow as it can be made: it admits `set_leds_16bit` and NOTHING else --
 * and only in one shape, all eight channels (mask 0x00FF, ten bytes) -- and
 * only while the grant is held. The ceiling is not consulted and not
 * changed, so the test ladder's order and consent rules are untouched.
 *
 * The grant is the operator's WAKE hold: granted when they wake him, revoked
 * when they put him down. It defaults to false.
 *
 * Refusals: R2_GATE_NOT_STATUS for any other op (checked first, whatever the
 * grant), R2_GATE_NOT_GRANTED while not granted, R2_GATE_NO_TX /
 * R2_GATE_ENCODE_FAILED as for r2_gate_send. Both paths share one admitted /
 * refused counter. */
void r2_gate_grant_status(bool granted);
bool r2_gate_status_granted(void);
int  r2_gate_send_status(uint8_t did, uint8_t cid, uint8_t seq,
                         const uint8_t *data, size_t data_len,
                         r2_tx_fn tx, void *ctx);

/* Human-readable, for logs and refusal messages. */
const char *r2_gate_tier_name(r2_tier_t t);
const char *r2_gate_verdict_name(r2_gate_verdict_t v);
/* How many sends the gate has admitted and refused since boot.
 *
 * Two jobs, and the second is why it is here now rather than in the telemetry
 * slice. It is the only observable a caller CANNOT forge: a module that
 * reimplements the gate's verdicts and encodes a frame itself can return the
 * right numbers, but it cannot move this counter. A test that asserts
 * "admitted advanced by exactly N, and exactly N frames went out" is therefore
 * a real proof of routing, where comparing return codes is not.
 *
 * Added after a mutation that faked r2_gate_send's own no-tx verdict survived
 * the slice 3 battery -- found by review, not by the tests. */
void r2_gate_stats(uint32_t *admitted, uint32_t *refused);
void r2_gate_stats_reset(void);

/* NULL when the op is not allowlisted. */
const char *r2_gate_op_name(uint8_t did, uint8_t cid);

#ifdef __cplusplus
}
#endif
#endif /* R2_GATE_H */
