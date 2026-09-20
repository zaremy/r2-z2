/* WHAT THE SERVICE MENU SAYS -- #101 child 6. Pure logic, no LVGL.
 *
 * The seven interiors behind SERVICE are the panel's "behind the door" half
 * (CLAUDE.md: indicator outside, diagnostics behind the door), which makes
 * them the place a person goes to find out what is wrong -- and therefore the
 * place a plausible invented value does the most damage. The v5 reference
 * fills every row with sample data ("HOMENET", "API KEY SET", "41°C"); this
 * module decides what THIS board can honestly put there, and that decision is
 * worth host tests in a way that drawing it is not.
 *
 * Three rules, and the tests pin each one:
 *   1. NO READING OUTLIVES ITS LINK (#101 AC7). Anything R2 told us goes to
 *      "----" the moment the link is not up, through r2_telemetry's own
 *      displayable() rather than a second copy of that rule.
 *   2. A THING THIS BUILD DOES NOT HAVE SAYS SO. No Wi-Fi stack, no API key,
 *      no camera, no microphone: the row reads NOT IN BUILD in the no-claim
 *      grey, never a sample value.
 *   3. THE LADDER NEVER BUNDLES (AC6). Every tier is its own rung, anything
 *      above the gate's ceiling is LOCKED, and there is no "all" rung.
 */
#ifndef PANEL_SERVICE_H
#define PANEL_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

#include "r2_telemetry.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The SERVICE rows, top to bottom. NETWORK is an interior like every other
 * row -- operator ruling 2026-09-19, which retired the lateral NETWORK page
 * the epic had it jump to. The reference's last row, EXIT TO OS, is absent
 * on purpose -- D-022 removed the vendor launcher, so there is no OS to exit
 * to, and a row that promised one would be a door painted on a wall. */
typedef enum {
    PANEL_SVC_NETWORK = 0,
    PANEL_SVC_R2_LINK,
    PANEL_SVC_DIAGNOSTICS,
    PANEL_SVC_HW_TEST,
    PANEL_SVC_PROVISIONING,
    PANEL_SVC_VOICE,
    PANEL_SVC_CAMERA,
    PANEL_SVC_ABOUT,
    PANEL_SVC_COUNT,
} panel_svc_t;

typedef enum {
    PANEL_SVC_NOTE = 0,    /* a null state: two lines, nothing to list. Zero,
                            * so a kind nobody set asks for no rows. */
    PANEL_SVC_LIST,        /* key / value rows */
    PANEL_SVC_LADDER,      /* HARDWARE TEST */
} panel_svc_kind_t;

/* What colour a value may claim. There is deliberately no RED: D-012 keeps it
 * for danger and stop, and nothing behind this door is either. NONE is the
 * panel's no-claim grey (D-012 Amendment A) -- absence, not a colour. */
typedef enum {
    PANEL_TONE_PLAIN = 0,  /* a fact, no verdict */
    PANEL_TONE_GOOD,       /* healthy */
    PANEL_TONE_WARN,       /* needs monitoring */
    PANEL_TONE_NONE,       /* we cannot say */
} panel_tone_t;

#define PANEL_SVC_VAL_LEN   16
#define PANEL_SVC_MAX_ROWS   8

typedef struct {
    const char  *key;
    char         val[PANEL_SVC_VAL_LEN];
    panel_tone_t tone;
} panel_kv_t;

/* Everything the rows are made from, gathered by the caller so this file
 * reads no clock and touches no hardware. */
typedef struct {
    const r2_telemetry_t *tm;
    uint32_t    now_ms;
    uint32_t    keepalive_ms;   /* main.c's keepalive period, not a copy of it */
    int         ceiling;        /* the gate's ceiling, as an r2_tier_t index */
    const char *panel_fw;       /* this app's version string */
    uint32_t    flash_mb, psram_mb;
    /* INTERNAL free heap. Total free counts PSRAM, which on this board is 8 MB
     * -- a number that reads reassuring while the internal heap, the one that
     * actually runs out, is small. Seen on the glass: FREE HEAP 7203 KB. */
    uint32_t    heap_kb;
    /* The touch controller's chip id as read at boot from I2C 0x15, or 0
     * when it did not answer. It is the board-revision probe
     * (board-revision.md): an answer at 0x15 IS a V2 board. */
    uint8_t     touch_id;
} panel_svc_facts_t;

/* THE BOARD'S OWN UPLINK, as the top chrome shows it -- panel_net's state,
 * named here so this file (which has host tests) owns what each step may
 * claim and panel_net owns only the mechanism. Zero is NONE: a chrome nobody
 * has told anything shows no claim, never a green one. */
typedef enum {
    PANEL_UPLINK_NONE = 0,     /* nothing provisioned, or no source at all */
    PANEL_UPLINK_JOINING,      /* trying: still not a claim */
    PANEL_UPLINK_WIFI,         /* associated, with an address */
    PANEL_UPLINK_CLOUD_OK,     /* the provider answered 2xx */
    PANEL_UPLINK_CLOUD_FAIL,   /* associated, provider unreachable */
} panel_uplink_t;

/* The tone of the Wi-Fi glyph and of the LLM word. Wi-Fi is GOOD from the
 * moment the board is associated -- that is a fact it holds -- and the LLM
 * word stays a no-claim grey until a request has actually come back 2xx:
 * being on a network is not evidence the provider is reachable. Anything out
 * of range is NONE, so a corrupt value cannot light either one. */
panel_tone_t panel_service_wifi_tone(panel_uplink_t u);
panel_tone_t panel_service_llm_tone(panel_uplink_t u);

const char       *panel_service_title(panel_svc_t s);   /* "" out of range */
panel_svc_kind_t  panel_service_kind(panel_svc_t s);

/* Fill a LIST interior's rows. Returns how many were written, never more than
 * `max`; 0 for anything that is not a LIST or when `f` is NULL. */
int panel_service_rows(panel_svc_t s, const panel_svc_facts_t *f,
                       panel_kv_t *out, int max);

/* A NOTE interior's two lines. False for anything that is not a NOTE. */
bool panel_service_note(panel_svc_t s, const char **line1, const char **line2);

/* The menu row's status square for R2 LINK: GOOD while linked, WARN otherwise.
 * The only menu row that makes a claim, because it is the only one whose
 * interior can go wrong while you are looking at the menu. */
panel_tone_t panel_service_link_tone(const r2_telemetry_t *tm);

/* THE PERMISSION LADDER, bring-up order: READ, LEDS, AUDIO, DOME, STANCE,
 * LOCOMOTION (CLAUDE.md). `ceiling` is the gate's tier index; every rung above
 * it is locked, an out-of-range ceiling locks everything, and LOCOMOTION is
 * locked at every ceiling because it is not a rung of the gate at all -- it
 * has no allowlist entry and is refused as unlisted. Returns the rung count. */
/* A CEILING IS NOT AN ORDER, WHICH IS WHAT THE RULE ACTUALLY SAYS (#168).
 * CLAUDE.md states bring-up as a SEQUENCE -- "read-only -> LEDs -> audio ->
 * small dome -> stance -> locomotion" -- and a ceiling only caps how far up
 * you may reach. At ceiling = STANCE the old ladder marked all five allowed,
 * so an operator could tap STANCE having never once run DOME: the rule's exact
 * prohibition. It was invisible while the ceiling sat at READ and only one
 * rung was tappable, which is why it had to be fixed before the first commit
 * that raises it.
 *
 * `done` is the set of tiers already exercised this session, bit i for tier i.
 * A rung opens only when the ceiling admits it AND every tier below it is in
 * that set. READ has nothing below it, so it opens whenever the ceiling
 * allows -- the sequence has to start somewhere.
 *
 * PER SESSION, NOT PERSISTED, and that is the safety-relevant half. A stored
 * "DOME ran fine" would unlock STANCE on a droid nobody has looked at since
 * last week -- and he has no resting posture, parks himself in bipod a minute
 * after the link drops, and an animation has put him on the floor once already
 * (CLAUDE.md). Re-walking the ladder after a boot costs one tap per rung and
 * re-proves what a stored bit only remembers.
 *
 * The rung says WHY it is shut, because "locked" and "not yet" ask different
 * things of the operator: one is a ceiling they must deliberately raise, the
 * other is a rung they have simply not reached. */
#define PANEL_LADDER_RUNGS 6
typedef enum {
    PANEL_RUNG_OPEN = 0,     /* tappable */
    PANEL_RUNG_CEILING,      /* above the gate's ceiling: raise it to reach this */
    PANEL_RUNG_SEQUENCE,     /* within the ceiling, but a lower tier is unrun */
    PANEL_RUNG_UNLISTED,     /* not a rung of the gate at all -- no ceiling admits it */
} panel_rung_block_t;
typedef struct {
    const char        *tier;
    bool               allowed;
    panel_rung_block_t why;      /* PANEL_RUNG_OPEN exactly when allowed */
} panel_rung_t;

/* Bit i marks tier i as exercised; `done` is a mask of these. The caller is
 * responsible for the range: this is a SHIFT, and `1u << -1` is undefined
 * behaviour, not a benign no-op. The one live call site passes a rung index
 * that is -1 at rest. */
_Static_assert(PANEL_LADDER_RUNGS <= 32,
               "a rung index wider than the mask would shift off the end");
#define PANEL_RUNG_BIT(tier) (1u << (tier))

int panel_service_ladder(int ceiling, uint32_t done,
                         panel_rung_t out[PANEL_LADDER_RUNGS]);

/* MAY THIS TIER'S TAP FIRE MORE THAN ONE OP? (#168 part 2)
 *
 * CLAUDE.md: "Each ACTUATOR test is individually opt-in, never bundled." The
 * word doing the work is ACTUATOR. READ asks him three questions -- battery,
 * dome position, firmware version -- and moves nothing, so one tap for three
 * reads breaks no rule.
 *
 * What it does do is establish TIER-AS-BUNDLE as the shape of the control,
 * and one rung up that shape is the prohibition: a STANCE tap that fires
 * whatever STANCE contains is exactly what the rule exists to prevent, and
 * D-010 put animations at that tier precisely because their contents cannot be
 * inspected first.
 *
 * So the bundle is not banned, it is CONDITIONAL, and the condition is a
 * property of the tier rather than of whoever writes the next one: a TEST of a
 * tier containing anything that commands motion, light or sound fires one op
 * per tap.
 *
 * A HALT IS EXEMPT, and the word TEST above is carrying that. r2_ops_stop_all
 * fires three ops on one tap -- two of them ANIMATRONIC -- and must, because a
 * partial stop is not a stop (D-026). Without this sentence the rule as
 * written makes the panel's own STOP button illegal, which is how a rule stops
 * being believed. The unit being rationed is CONSENT TO MAKE HIM ACT; a
 * command to stop acting is not more of it.
 *
 * READ is the only tier this returns true for, and it says why in the table
 * rather than by being special-cased at the top.
 *
 * WHAT THIS IS NOT. It is not per-op consent. The rule's words are
 * "individually OPT-IN", and opting in means the operator sees a named thing
 * and chooses it; this only caps how many unnamed things one tap may fire. A
 * moving tier still needs its ops drawn as rows -- D-025's consequence list
 * calls that "rebuilds these rows for per-op consent" and it is not done here.
 * What is done is that the bundle cannot be the default when that work lands.
 *
 * AND THE FALSE BRANCH IS UNREACHABLE IN THIS BUILD. run_tier_test refuses
 * every tier but READ before it reaches the budget, so `may_bundle` is
 * constant-true there today and a compiler may fold the whole thing away.
 * This is a marker for whoever raises the ceiling, not a control that is
 * currently protecting anything -- said plainly because a guard that cannot
 * fire reads exactly like one that can. */
bool panel_service_tier_may_bundle(int tier);

/* THE TIER'S OPS, BY NAME (#168 part 2, the other half).
 *
 * `panel_service_tier_may_bundle` capped how many UNNAMED ops one tap may
 * fire. That is not consent, it is rationing. CLAUDE.md's words are "each
 * actuator test is individually OPT-IN", and opting in means the operator
 * sees a named thing and chooses that thing. This is the catalogue that lets
 * them: a rung no longer runs a tier, it opens the tier's ops, and each one is
 * its own tap.
 *
 * A TIER WITH NO CATALOGUE RETURNS 0, AND 0 MEANS REFUSED -- the same shape as
 * panel_probe_timeout_ms, and for the same reason. Today only READ has one,
 * because r2_ops can emit READ's three questions and the LED writes and
 * nothing else; inventing a plausible row for AUDIO or STANCE would put a
 * named, tappable control on the glass for an op no code sends. A guessed
 * catalogue reads exactly like a measured one six months from now. Whoever
 * raises the ceiling to a tier writes that tier's rows, beside the timeout
 * they must also go and measure.
 *
 * THE BUNDLE IS A ROW, AND ONLY WHERE IT IS LEGAL. READ may bundle, so its
 * list leads with RUN ALL 3 -- three reads on one tap, which breaks no rule
 * and is what the ladder did before. An actuator tier never gets that row, and
 * that is the invariant worth pinning: it is the rule itself expressed as
 * data, and the test asserts it across every tier rather than only the one
 * that exists today. */
#define PANEL_TIER_OPS_MAX 4

typedef enum {
    PANEL_OP_ALL = 0,     /* every op below, on one tap. Bundling tiers only. */
    PANEL_OP_BATTERY,     /* DID_POWER 0x03 */
    PANEL_OP_HEAD,        /* DID_ANIMATRONIC 0x14 -- reads the dome, never turns it */
    PANEL_OP_VERSION,     /* DID_SYSTEM_INFO 0x00 */
    PANEL_OP__COUNT,
} panel_op_t;

/* LONG ENOUGH FOR THE LONGEST ROW, checked against the shipped table below.
 * "DOME POSITION" is 13. */
#define PANEL_OP_NAME_LEN 20

typedef struct {
    panel_op_t  op;
    /* WHAT THE ROW SAYS, COPIED IN RATHER THAN POINTED AT. The bundle row's
     * label is BUILT ("RUN ALL 3"), so a `const char *` had to point into a
     * static buffer that the next call rewrites -- a row a caller held would
     * silently change its own label with the holder doing nothing, which a
     * probe demonstrated. Every other name is a literal and would have been
     * fine; one of them was not, and a rule that holds for three rows out of
     * four is not a rule. Always NUL-terminated, never empty in a returned
     * row. */
    char        name[PANEL_OP_NAME_LEN];
    /* Does firing THIS row act on him? Per-op, not per-tier: a tier counts as
     * an actuator tier when ANY of its ops moves something, and the rows
     * inside it are not all alike -- a future DOME list holds both a read-back
     * and a turn. The renderer colours on this, so a row that moves him cannot
     * be drawn like one that asks a question. */
    bool        moves;
    /* HOW MANY OPS THIS ROW SENDS, which is what a PASS has to count.
     *
     * THE BUNDLE ROW'S FIELD IS THE ONLY INTERESTING ONE, and pretending
     * otherwise was a mistake worth recording: an earlier version let a single
     * row claim any count, and a test was written to prove the bundle SUMS
     * them rather than counting rows. `run_op` ignores the field for singles
     * -- it maps a named op to exactly one send -- so that distinction existed
     * nowhere but in the struct, and the fixture invented to test it was
     * testing a property the system does not have.
     *
     * Worse, a single row with sends > 1 IS a bundled test wearing a name:
     * "ALL LEDS OFF" firing five commands on one tap is what CLAUDE.md
     * forbids, reached without ever touching the RUN ALL row.
     *
     * So a single row sends exactly 1, refused otherwise, and the bundle's
     * field is the number of rows below it. The sum and the count are now the
     * same number BECAUSE THEY ARE THE SAME THING -- which makes a mutation
     * swapping one for the other equivalent rather than surviving, and that is
     * the honest version of a result this PR first claimed by fixture. */
    uint8_t     sends;
} panel_tier_op_t;

/* Fill a tier's op rows, in tap order. Returns how many were written: 0 for a
 * tier with no catalogue, which the caller must treat as "this rung cannot be
 * run", never as "run it with no ops". */
int panel_service_tier_ops(int tier, panel_tier_op_t out[PANEL_TIER_OPS_MAX]);

/* THE RULE, LIFTED OUT FROM BEHIND THE TABLE, and this is not tidiness.
 *
 * Only READ has a catalogue, so every actuator tier returns 0 rows -- which
 * means the invariant that matters, "a tier that moves him is never given a
 * bundle row", is UNFALSIFIABLE through panel_service_tier_ops. The assertion
 * passes on an empty list, and would pass just as happily with the rule
 * deleted. That is the repo's own mutation finding wearing a new coat: the
 * test asserts the shipped data is fine rather than that the check rejects bad
 * input.
 *
 * So the check lives here, tier and catalogue both arguments, and
 * panel_service_tier_ops is the table plus one call to it. A test can hand
 * this STANCE and three ops and watch it refuse the bundle row -- today,
 * against a tier that does not exist yet, which is the only moment the rule
 * can be proved before it is load-bearing.
 *
 * `n_ops` above PANEL_TIER_OPS_MAX minus the bundle row is refused outright
 * rather than truncated: a list missing its last row is a control the operator
 * cannot reach and cannot tell is missing. */
int panel_service_ops_rows(int tier, const panel_tier_op_t *ops, int n_ops,
                           panel_tier_op_t out[PANEL_TIER_OPS_MAX]);

/* HAS THIS TIER BEEN EXERCISED? -- and per-op consent changes the answer.
 *
 * The sequence gate (D-025) opens a rung only when every tier below it has
 * run, and while a tap fired the whole tier that question had one meaning.
 * Split into named ops it has three possible ones, and two of them are wrong:
 *
 *   - "any op passed" would let ONE of READ's three questions unlock LEDS.
 *     The bit would mean "something answered", which is what the ledger work
 *     in #174 was done to stop it meaning.
 *   - "the bundle passed" would make the gate unreachable on any tier that is
 *     not allowed a bundle -- which is every tier that moves him, i.e. every
 *     tier the gate actually protects.
 *
 * So: the bundle row passing satisfies it on its own, because a PASS there
 * already means every op it contains was answered; otherwise EVERY single-op
 * row must have passed, in whatever order the operator chose. That is the
 * sequence rule surviving contact with per-op consent rather than being
 * quietly widened by it.
 *
 * `passed` is a mask over the rows panel_service_tier_ops returned, bit i for
 * row i. An empty list is NOT exercised -- a tier nobody catalogued cannot
 * have run. */
bool panel_service_tier_exercised(const panel_tier_op_t *rows, int n,
                                  uint32_t passed);

/* Does this tier drive anything physical? An out-of-range tier answers TRUE --
 * an unknown rung is treated as if it moves him, because the safe default for
 * a question about actuators is yes. */
bool panel_service_tier_is_actuator(int tier);


/* The ceiling's name for the ladder header, "" when out of range. */
const char *panel_service_ceiling_name(int ceiling);

#ifdef __cplusplus
}
#endif
#endif /* PANEL_SERVICE_H */
