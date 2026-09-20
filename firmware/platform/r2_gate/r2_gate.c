#include <stdbool.h>
#include <string.h>

#include "r2_gate.h"
#include "r2_packet.h"

/* Wire constants, all cited to mac-prototype/r2_probe.py:76-99, which is the
 * validated source for them. */
#define DID_POWER       0x13
#define DID_ANIMATRONIC 0x17
#define DID_IO          0x1A
#define DID_SENSOR      0x18
#define DID_SYSTEM_INFO 0x11
#define DID_DRIVE       0x16

typedef struct { uint8_t did, cid; r2_tier_t tier; const char *name; } op_t;

/* ---- ALLOWLIST. Anything absent is refused. ------------------------------ */
static const op_t ALLOWED[] = {
    /* read — cannot move him, which is the entire test for this tier */
    { DID_POWER,       0x0D, R2_TIER_READ,  "wake" },            /* the keepalive */
    { DID_POWER,       0x03, R2_TIER_READ,  "battery_voltage" },
    { DID_ANIMATRONIC, 0x14, R2_TIER_READ,  "get_head_position" },
    { DID_ANIMATRONIC, 0x25, R2_TIER_READ,  "get_leg_action" },
    { DID_ANIMATRONIC, 0x16, R2_TIER_READ,  "get_leg_position" },
    { DID_ANIMATRONIC, 0x2B, R2_TIER_READ,  "stop_animation" },  /* a stop is always safe */
    { DID_SENSOR,      0x01, R2_TIER_READ,  "get_sensor_mask" },
    { DID_SYSTEM_INFO, 0x00, R2_TIER_READ,  "get_main_app_version" },

    /* leds */
    { DID_IO,          0x0E, R2_TIER_LEDS,  "set_leds_16bit" },  /* the variant R2D2 exposes */

    /* audio */
    { DID_IO,          0x07, R2_TIER_AUDIO, "play_audio" },
    { DID_IO,          0x08, R2_TIER_AUDIO, "set_volume" },
    { DID_IO,          0x0A, R2_TIER_AUDIO, "stop_audio" },

    /* dome */
    { DID_ANIMATRONIC, 0x0F, R2_TIER_DOME,  "set_head_position" },
};
#define ALLOWED_N (sizeof ALLOWED / sizeof ALLOWED[0])

/* ---- FORBIDDEN at every ceiling, checked FIRST. --------------------------
 * Named rather than merely omitted, so adding one to the allowlist by mistake
 * still cannot get it out. Each is here for a recorded reason. */
static const op_t FORBIDDEN[] = {
    /* Undoes itself in 3 s against our own keepalive, and D-023 makes release a
     * SUBTRACTION -- we stop sending, we never send this. */
    { DID_POWER,       0x01, R2_TIER_READ, "sleep" },
    /* Sphero deep sleep typically needs the charger to exit. */
    { DID_POWER,       0x00, R2_TIER_READ, "enter_deep_sleep" },
    /* D-010: an animation is a stance command whose contents cannot be
     * inspected first. EMOTE_YES emitted WADDLE three times and put him on the
     * floor with perform_leg_action never called by us. */
    { DID_ANIMATRONIC, 0x05, R2_TIER_READ, "play_animation" },
    /* EXCEPT with a payload of exactly {LEG_ACTION_STOP}, which the HALTS
     * list below admits before this one is consulted (D-026). The ban is on
     * the MOTION; the halt shares its CID and could not be told apart until
     * the gate learned to read the payload. */
    { DID_ANIMATRONIC, 0x0D, R2_TIER_READ, "perform_leg_action" },
    /* #22 AC6: the float is finer-grained than the stance enum and documented
     * nowhere. A write is a guess at an actuator that can fell him. */
    { DID_ANIMATRONIC, 0x15, R2_TIER_READ, "set_leg_position" },
    /* Locomotion is a separate decision and a separate tier (#23). */
    { DID_DRIVE,       0x07, R2_TIER_READ, "drive" },
};
#define FORBIDDEN_N (sizeof FORBIDDEN / sizeof FORBIDDEN[0])

/* ---- HALTS, admitted at EVERY ceiling, checked BEFORE everything else. ----
 *
 * "Default to STOP. Disconnect or failure must result in stop, not
 * last-command" (CLAUDE.md). A panel that cannot send a stop does not satisfy
 * that at any ceiling, and until now it could not send a complete one: the
 * legs halt was FORBIDDEN outright and the audio halt sat behind the AUDIO
 * ceiling, so the stop got weaker exactly as the tiers got more dangerous.
 *
 * The Mac prototype learned the shape the hard way and says so at
 * `stop_everything`: the legs halt was "added after #11 -- an animation drives
 * LEG actions, and stop_animation is not documented to halt one already in
 * flight. A stop that leaves the legs moving is not a stop -- and a leg action
 * in flight is the state that put R2 on the floor."
 *
 * WHY THIS DOES NOT REOPEN WHAT FORBIDDEN CLOSED. `perform_leg_action` is
 * forbidden because "an animation is a stance command whose contents cannot be
 * inspected first" (D-010). That objection is about contents -- and here the
 * contents are pinned: this entry admits DID 0x17 / CID 0x0D only when the
 * payload is EXACTLY one byte, LEG_ACTION_STOP. Any other leg action, of any
 * length, falls through to FORBIDDEN and is refused as before. The gate could
 * not previously express that distinction because it only ever saw did+cid,
 * which is why the halt had to be banned along with the motion.
 *
 * Each entry pins its exact bytes. That is the property that makes this list
 * safe to check first: it cannot widen anything by accident, because nothing
 * matches it approximately. */
#define LEG_ACTION_STOP 0x00u

typedef struct {
    uint8_t     did, cid;
    const uint8_t *data;      /* the ONLY payload this admits */
    size_t      data_len;
    const char *name;
} halt_t;

static const uint8_t LEG_STOP_PAYLOAD[] = { LEG_ACTION_STOP };

static const halt_t HALTS[] = {
    /* Halts an animation already playing. No payload: nothing to constrain. */
    { DID_ANIMATRONIC, 0x2B, NULL, 0, "stop_animation" },
    /* Halts audio. Was allowlisted at the AUDIO tier, which meant the panel
     * could not silence him from READ -- a stop you have to raise a ceiling to
     * reach is not a stop. It stays in ALLOWED too; this is the always-path. */
    { DID_IO,          0x0A, NULL, 0, "stop_audio" },
    /* THE ONE WITH A PINNED PAYLOAD. Legs stop, and only legs stop. */
    { DID_ANIMATRONIC, 0x0D, LEG_STOP_PAYLOAD, sizeof LEG_STOP_PAYLOAD,
      "perform_leg_action(STOP)" },
};
#define HALTS_N (sizeof HALTS / sizeof HALTS[0])

/* EXACT BYTES, not a prefix. A longer payload whose first byte happens to be
 * LEG_ACTION_STOP is a different command and is not a halt. */
static bool is_halt(uint8_t did, uint8_t cid,
                    const uint8_t *data, size_t data_len)
{
    for (size_t i = 0; i < HALTS_N; i++) {
        if (HALTS[i].did != did || HALTS[i].cid != cid) continue;
        if (HALTS[i].data_len != data_len) continue;
        if (data_len == 0) return true;
        if (data == NULL) continue;
        if (memcmp(data, HALTS[i].data, data_len) == 0) return true;
    }
    return false;
}

static r2_tier_t s_ceiling = R2_TIER_READ;   /* lowest rung by default */

/* The status grant (D-030). False by default, so a firmware that never
 * grants can never light him through this path. */
static bool s_status_granted = false;

static uint32_t s_admitted = 0, s_refused = 0;

void r2_gate_stats(uint32_t *admitted, uint32_t *refused)
{
    if (admitted) *admitted = s_admitted;
    if (refused)  *refused  = s_refused;
}

void r2_gate_stats_reset(void) { s_admitted = s_refused = 0; }

void r2_gate_set_ceiling(r2_tier_t c)
{
    /* An out-of-range ceiling clamps DOWN, never up: a bad value must not be a
     * privilege escalation. */
    if (c >= R2_TIER__COUNT) c = R2_TIER_READ;
    s_ceiling = c;
}

r2_tier_t r2_gate_get_ceiling(void) { return s_ceiling; }

static const op_t *find(const op_t *tbl, size_t n, uint8_t did, uint8_t cid)
{
    for (size_t i = 0; i < n; i++)
        if (tbl[i].did == did && tbl[i].cid == cid) return &tbl[i];
    return NULL;
}

r2_gate_verdict_t r2_gate_check(uint8_t did, uint8_t cid,
                                const uint8_t *data, size_t data_len)
{
    /* FIRST, and above FORBIDDEN. A halt is the one thing that must never be
     * refused for being too dangerous: refusing it leaves him moving. */
    if (is_halt(did, cid, data, data_len)) return R2_GATE_ALLOW;
    if (find(FORBIDDEN, FORBIDDEN_N, did, cid)) return R2_GATE_FORBIDDEN;
    const op_t *op = find(ALLOWED, ALLOWED_N, did, cid);
    if (op == NULL) return R2_GATE_NOT_ALLOWLISTED;
    if (op->tier > s_ceiling) return R2_GATE_ABOVE_CEILING;
    return R2_GATE_ALLOW;
}

/* Encode and transmit an op that has ALREADY been admitted. Static, so the
 * only ways to reach it are the two public exits below, each of which has
 * decided first. */
static int transmit(uint8_t did, uint8_t cid, uint8_t seq,
                    const uint8_t *data, size_t data_len,
                    r2_tx_fn tx, void *ctx)
{
    if (tx == NULL) { s_refused++; return R2_GATE_NO_TX; }

    uint8_t frame[R2_ENCODED_MAX(64)];
    const int n = r2_packet_encode(did, cid, seq, data, data_len,
                                   frame, sizeof frame);
    if (n <= 0) { s_refused++; return R2_GATE_ENCODE_FAILED; }
    /* Counted BEFORE the transport, deliberately. A tx that returns an error
     * may still have put the bytes on the air, and this project's rule is to
     * assume an unconfirmed command took effect. Counting after would let a
     * failed-but-delivered send go unrecorded, which is the wrong direction to
     * be wrong in for the one counter that says what we sent him. */
    s_admitted++;
    if (tx(frame, (size_t)n, ctx) < 0) return R2_GATE_ENCODE_FAILED;
    return n;
}

int r2_gate_send(uint8_t did, uint8_t cid, uint8_t seq,
                 const uint8_t *data, size_t data_len,
                 r2_tx_fn tx, void *ctx)
{
    /* Refuse BEFORE encoding. Nothing should build a frame it may not send --
     * a half-built forbidden command is a thing waiting to be transmitted by
     * the next person who adds a shortcut. */
    const r2_gate_verdict_t v = r2_gate_check(did, cid, data, data_len);
    if (v != R2_GATE_ALLOW) { s_refused++; return (int)v; }
    return transmit(did, cid, seq, data, data_len, tx, ctx);
}

void r2_gate_grant_status(bool granted) { s_status_granted = granted; }
bool r2_gate_status_granted(void)       { return s_status_granted; }

int r2_gate_send_status(uint8_t did, uint8_t cid, uint8_t seq,
                        const uint8_t *data, size_t data_len,
                        r2_tx_fn tx, void *ctx)
{
    /* The op test comes FIRST, so a caller that tries to smuggle anything
     * else through this path is told what it did wrong whether or not the
     * grant happens to be held. Exactly one op, compared by value -- not
     * "anything at the LEDS tier", which would widen by itself the day a
     * second op is added to that tier. */
    if (did != DID_IO || cid != 0x0E) { s_refused++; return R2_GATE_NOT_STATUS; }
    /* AND ITS SHAPE, pinned like the halts pin theirs: every channel, mask
     * 0x00FF, eight values. A partial mask would leave an earlier state's
     * fixture lit; any other length is a different packet. */
    if (data == NULL || data_len != 10 || data[0] != 0x00 || data[1] != 0xFF) {
        s_refused++; return R2_GATE_NOT_STATUS;
    }
    if (!s_status_granted)            { s_refused++; return R2_GATE_NOT_GRANTED; }
    return transmit(did, cid, seq, data, data_len, tx, ctx);
}

/* ---- THE REPLY PATH (D-032, E2E v0 slice 3.4c/3.5a's audio half) ---------
 *
 * THE COMMITTED TABLE. Ids from mac-prototype/r2_assets.py, kept by the 3.4a
 * chirp survey (docs/research/r2-capabilities.md, "3.4a chirp survey"; raw
 * ratings in mac-prototype/results/chirp_survey_3.4a.jsonl). Not mood-keyed
 * here -- that mapping is r2_ops_chirp_id_for_mood()'s job (r2_ops.c); this
 * table only says which ids are safe to say at all, the same separation the
 * LEDs keep between "what channel" (r2_ops) and "is the write well-formed"
 * (here). Kept flat rather than five one-entry tables so a future revision
 * that widens a mood to its second surviving candidate needs no gate change. */
static const uint16_t REPLY_CHIRP_IDS[] = {
    1966, /* R2_CHATTY_11  curious */
    2007, /* R2_CHATTY_15  curious */
    3302, /* R2_POSITIVE_1 happy   */
    1910, /* R2_ANNOYED    annoyed */
    3101, /* R2_NEGATIVE_1 annoyed */
    3484, /* R2_SAD_1      sad     */
    3703, /* R2_SAD_5      sad     */
    1737, /* R2_ALARM_1    alert   */
    2813, /* R2_HEY_1      alert   */
};
#define REPLY_CHIRP_IDS_N (sizeof REPLY_CHIRP_IDS / sizeof REPLY_CHIRP_IDS[0])

static bool is_reply_chirp_id(uint16_t id)
{
    for (size_t i = 0; i < REPLY_CHIRP_IDS_N; i++)
        if (REPLY_CHIRP_IDS[i] == id) return true;
    return false;
}

/* PLAY_IMMEDIATELY (spherov2 commands/io.py AudioPlaybackModes.0): a reply
 * chirp never needs to queue, since this door admits at most one per
 * exchange. */
#define AUDIO_PLAY_IMMEDIATELY 0x00u

/* The id that has already spent its one chirp. 0 is safe as "none yet": it is
 * panel_exchange's own "no live exchange" sentinel (panel_exchange.h), so a
 * real exchange id can never legitimately collide with the initialiser. */
static uint32_t s_chirp_used_for_exchange = 0;

int r2_gate_send_reply_audio(bool may_act, uint32_t exchange_id, uint16_t sound_id,
                             uint8_t seq, r2_tx_fn tx, void *ctx)
{
    /* ILLEGAL CASES FIRST (CLAUDE.md, "mutate the guard, not the table").
     * The grant first: it is the one D-032 says is shared with D-030, so a
     * reader checking "is he even awake" finds the same answer either door
     * gives. */
    if (!s_status_granted)                        { s_refused++; return R2_GATE_NOT_GRANTED; }
    if (!may_act)                                  { s_refused++; return R2_GATE_REPLY_NOT_LIVE; }
    if (!is_reply_chirp_id(sound_id))              { s_refused++; return R2_GATE_REPLY_BAD_ID; }
    if (exchange_id == s_chirp_used_for_exchange)  { s_refused++; return R2_GATE_REPLY_USED; }

    const uint8_t payload[3] = {
        (uint8_t)(sound_id >> 8), (uint8_t)(sound_id & 0xFFu),
        (uint8_t)AUDIO_PLAY_IMMEDIATELY,
    };
    const int n = transmit(DID_IO, 0x07 /* play_audio */, seq,
                           payload, sizeof payload, tx, ctx);
    /* Spend the budget only on a definite success. A NO_TX or an encode
     * failure never put bytes on the air, so the exchange's one chirp is
     * still owed -- unlike the admitted/refused counters above, which count a
     * transmit() attempt before the transport even runs (see transmit()'s own
     * comment, "assume an unconfirmed command took effect"). Those two
     * philosophies differ on purpose: the counter is allowed to over-count
     * what MIGHT have gone out, but a real second chirp reaching the radio is
     * the failure this door exists to prevent, so the budget only moves on
     * n > 0. */
    if (n > 0) s_chirp_used_for_exchange = exchange_id;
    return n;
}

const char *r2_gate_tier_name(r2_tier_t t)
{
    switch (t) {
    case R2_TIER_READ:   return "read";
    case R2_TIER_LEDS:   return "leds";
    case R2_TIER_AUDIO:  return "audio";
    case R2_TIER_DOME:   return "dome";
    case R2_TIER_STANCE: return "stance";
    default:             return "?";
    }
}

const char *r2_gate_verdict_name(r2_gate_verdict_t v)
{
    switch (v) {
    case R2_GATE_ALLOW:           return "allow";
    case R2_GATE_ABOVE_CEILING:   return "refused: above the ceiling";
    case R2_GATE_NOT_ALLOWLISTED: return "refused: not allowlisted";
    case R2_GATE_FORBIDDEN:       return "refused: forbidden at every ceiling";
    case R2_GATE_NO_TX:           return "refused: no transmit function";
    case R2_GATE_ENCODE_FAILED:   return "refused: encode or transmit failed";
    case R2_GATE_NOT_GRANTED:     return "refused: the WAKE grant is not held";
    case R2_GATE_NOT_STATUS:      return "refused: not a status-light op";
    case R2_GATE_REPLY_NOT_LIVE:  return "refused: this exchange may not act";
    case R2_GATE_REPLY_BAD_ID:    return "refused: not a committed chirp id";
    case R2_GATE_REPLY_USED:      return "refused: this exchange already chirped";
    default:                      return "?";
    }
}

const char *r2_gate_op_name(uint8_t did, uint8_t cid)
{
    const op_t *op = find(ALLOWED, ALLOWED_N, did, cid);
    if (op) return op->name;
    op = find(FORBIDDEN, FORBIDDEN_N, did, cid);
    return op ? op->name : NULL;
}
