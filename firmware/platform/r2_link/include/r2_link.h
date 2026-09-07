/* The BLE link to R2 -- #114 slice 4.
 *
 * WHY THIS EXISTS
 *
 * Slices 1-3 shipped 1,095 host checks and had never sent a byte to the droid.
 * That is the exact shape of the failure CLAUDE.md records: four LED PRs merged
 * completely inert because nothing ever called them, and it took the operator
 * asking why a shipped thing was invisible to find out. Correctness does not
 * detect deadness. This slice is the one call site that makes the stack real.
 *
 * WHAT IT IS
 *
 * A thin seam over NimBLE that connects to R2 and exposes exactly two things:
 * a send function shaped as an r2_tx_fn, and a callback that hands up whole
 * Sphero frames. Everything above it -- framing, the permission ceiling, the
 * ops -- is the already-tested platform layer.
 *
 * WHAT IT REPLACES, AND THE BUG THAT COMES WITH IT
 *
 * coex_check/main/r2_central.c built its packets by hand in three places and
 * escaped none of them. A Sphero frame must escape 0x8D, 0xD8 and 0xAB
 * anywhere in the body, and the sequence byte walks through all three every
 * 256 packets. Across A1's 3,778 keepalives that is roughly 45 malformed
 * frames, and the instrument could not see a single one: coex_stats records
 * the ATT write status, and the ATT write SUCCEEDS -- R2 accepts the bytes at
 * the link layer and then discards the packet at the Sphero layer. A keepalive
 * that silently fails to wake him looks identical to one that worked.
 *
 * It was harmless there because the next keepalive is three seconds behind.
 * It would not be harmless on a command that matters. Routing every send
 * through r2_packet_encode fixes it structurally rather than by remembering to.
 */
#ifndef R2_LINK_H
#define R2_LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    R2_LINK_DOWN = 0,     /* not connected, not looking */
    R2_LINK_SCANNING,     /* looking for a name starting "D2-" */
    R2_LINK_CONNECTING,   /* GAP connect in flight */
    R2_LINK_HANDSHAKING,  /* connected; magic write and discovery in progress */
    R2_LINK_UP,           /* command characteristic found, subscribed, usable */
} r2_link_state_t;

typedef struct {
    /* reason is the NimBLE disconnect reason on a fall to DOWN, else 0. */
    void (*on_state)(r2_link_state_t state, int reason, void *ctx);
    /* One complete Sphero frame, SOP..EOP, still escaped. Feed it to
     * r2_packet_decode. Called from the NimBLE host task -- do not block. */
    void (*on_frame)(const uint8_t *frame, size_t len, void *ctx);
    void *ctx;
} r2_link_cbs_t;

/* Callbacks may be NULL individually; cbs itself may not. */
void r2_link_init(const r2_link_cbs_t *cbs);

/* Begin scanning. Reconnects on its own after a disconnect. */
void r2_link_start(void);

bool            r2_link_is_up(void);
r2_link_state_t r2_link_state(void);
const char     *r2_link_state_name(r2_link_state_t s);

/* Deliberately shaped as r2_tx_fn so it can be handed straight to
 * r2_gate_send(). ctx is ignored. Returns 0 on success, negative on failure --
 * and note what "success" means here: the frame was queued to NimBLE. It is
 * not proof R2 received or accepted it, which is why this project's rule is to
 * assume an unconfirmed command took effect. */
int r2_link_send(const uint8_t *frame, size_t len, void *ctx);

/* Scan for him but never connect (#101 P4).
 *
 * His idle timeout is unmeasured, and it decides how long `released` sits in
 * limbo (D-023). It cannot be measured while we are connected, because OUR
 * KEEPALIVE IS HIS WAKE COMMAND -- CLAUDE.md is explicit that "no idle timeout
 * in practice" is us suppressing his own, every three seconds.
 *
 * So this mode watches the advertisement and never opens a link. Note what it
 * can and cannot conclude: the only sleep this project has ever OBSERVED was
 * visual (he reverted to his resting alternation and faded out). Whether a
 * sleeping droid stops advertising is UNKNOWN, so continued advertising is not
 * evidence he is awake -- it is an absence of evidence either way. */
void r2_link_set_scan_only(bool on);

/* How many adverts we have seen from him, and when the last one arrived. */
void r2_link_adverts(uint32_t *count, uint32_t *last_ms);

/* Drop the link deliberately. Returns 0 if a disconnect was started.
 *
 * Two callers, present and future. Here it is how the telemetry rule gets
 * PROVEN on hardware rather than only in host tests: a reading cannot be shown
 * to outlive its link unless something can make the link end on demand.
 *
 * And it is the primitive D-023's panel RELEASE control needs. That ADR's
 * finding was that powering him down is us STOPPING -- our keepalive is his
 * wake command, so he has no idle timeout in practice because we suppress it
 * every three seconds. Releasing him is letting go, and this is the letting
 * go. It is deliberately NOT called "sleep": sleep (DID 0x13 / CID 0x01) is a
 * command in the gate's FORBIDDEN table at every tier, and this sends nothing
 * to R2 at all. */
int r2_link_disconnect(void);

/* Frames queued, and frames refused because the link was down. */
void r2_link_stats(uint32_t *sent, uint32_t *dropped);

#ifdef __cplusplus
}
#endif
#endif /* R2_LINK_H */
