/* The panel becomes the droid's own screen — #101 children 2 and 3.
 *
 * Child 2: it boots as the DEVICE'S OWN SURFACE, on our partition table
 * (D-022), with no vendor launcher underneath and nothing to return to
 * (D-021 -- we own idle).
 *
 * Child 3: the resting STATUS frame on real glass.
 *
 * And child 7 does not exist any more. The epic budgeted "live telemetry
 * replaces mock" as a late child because it assumed the panel would be built
 * against fabricated data for weeks first. #114 landed the link before the UI,
 * so the mock layer was never written: these rows have never shown anything
 * but R2. That was the stated reason for doing S5 first, and it is the cheaper
 * order -- a rendering bug can be told apart from a data bug.
 *
 * NOT ESP-BROOKESIA. #101 child 1, decided: plain LVGL 9. Its value was "our
 * app beside the vendor's" and D-022 removed the vendor's.
 *
 * SAFETY: the gate ceiling is never raised. This panel reads; it cannot move
 * him, light him, or sound him.
 */
#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "lvgl.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs_flash.h"

#include "panel_probe.h"
#include "panel_service.h"
#include "panel_shot.h"
#include "panel_touch.h"
#include "panel_ui.h"
#include "r2_gate.h"
#include "r2_link.h"
#include "r2_ops.h"
#include "r2_packet.h"
#include "r2_telemetry.h"

static const char *TAG = "panel";

static r2_telemetry_t s_tm;
static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* 0..254, NEVER 0xFF. R2 stamps its own unsolicited notifications with seq
 * 0xFF -- head_reset_to_zero, leg_action_complete, animation_complete -- and
 * the Mac prototype has reserved it from the start for exactly that reason:
 * "Our own sequence counter is `% 0xFF` ... so it can never collide with 0xFF.
 * That is what makes 'unmatched' a safe test for 'robot-initiated'."
 * (r2_probe.py:226). This counter walked all 256, so roughly one test in
 * eighty held 0xFF and a notification could strike a question R2 never heard
 * -- on DOME and STANCE, the two tiers that EMIT those notifications and the
 * two the sequence gate exists to protect. */
static uint8_t next_seq(void) { static uint8_t s; s = (uint8_t)((s + 1u) % 0xFFu); return s; }

/* Asked once per connection, cleared when the link falls. Written by the UI
 * loop and cleared on the NimBLE host task, hence volatile: the worst
 * interleaving costs one duplicate probe, never a missed one. */
static volatile bool s_version_asked;

/* WHAT THE RUNNING TEST ASKED, STRUCK OFF AS ITS OWN REPLIES ARRIVE (#168).
 *
 * THREE TASKS TOUCH THIS, which is why it takes a real lock and not the
 * `volatile` above. `link_task` fills it when a test's ops go out; the NimBLE
 * host task strikes entries in on_frame(); ui_task reads the count to decide
 * the verdict. It is an array plus two counters -- a torn read here is not one
 * stale value for one frame, it is a verdict computed from half an update.
 *
 * The critical sections are a handful of byte compares over at most eight
 * entries, taken on the radio's own callback. That is short enough to sit in a
 * spinlock and nowhere near long enough to hold off a higher-priority task. */
static portMUX_TYPE s_ledger_mux = portMUX_INITIALIZER_UNLOCKED;
static panel_ledger_t s_ledger;

/* Every reply carries back the seq it answers. Returns whether it struck one
 * of the running test's questions -- a poll, a keepalive or an echo does not.*/
static bool ledger_note(uint8_t seq)
{
    portENTER_CRITICAL(&s_ledger_mux);
    const bool mine = panel_ledger_note(&s_ledger, seq);
    portEXIT_CRITICAL(&s_ledger_mux);
    return mine;
}

unsigned panel_main_test_answered(void)
{
    portENTER_CRITICAL(&s_ledger_mux);
    const unsigned n = panel_ledger_answered(&s_ledger);
    portEXIT_CRITICAL(&s_ledger_mux);
    return n;
}

static void on_state(r2_link_state_t s, int reason, void *ctx)
{
    (void)ctx; (void)reason;
    if (s != R2_LINK_UP) s_version_asked = false;
    /* The telemetry layer forgets every reading here. That is what keeps a
     * voltage from outliving the link that carried it, and it is why the R2
     * row goes to "--" rather than holding the last good number. */
    r2_telemetry_link(&s_tm, (r2_tm_link_t)s, now_ms());
    ESP_LOGI(TAG, "link -> %s", r2_link_state_name(s));
}

static void on_frame(const uint8_t *frame, size_t len, void *ctx)
{
    (void)ctx;
    uint8_t scratch[64];
    r2_response_t r;
    if (r2_packet_decode(frame, len, scratch, sizeof scratch, &r) != R2_OK) return;
    const bool is_response = (r.flags & R2_FLAG_IS_RESPONSE) != 0u;

    /* A FRAME IS NOT AN ANSWER. Three things have to be true before this
     * strikes a question off the running test, and each was a real hole:
     *
     *   IT MUST BE A RESPONSE. An unsolicited notification carries a seq too
     *   (r2_packet.c populates it either way), so matching on seq alone let
     *   robot-initiated traffic resolve a live waiter. The Mac prototype hit
     *   this and gated on the flag; so do we.
     *
     *   IT MUST NOT BE AN ERROR. r2_ops refuses a reply with err != 0
     *   (R2_OPS_DEVICE_ERROR), so the counter this replaced could never be
     *   credited by a refusal -- the stamp was only written after a good
     *   parse. Striking before the parse was strictly WEAKER than the code it
     *   replaced: a STANCE command R2 refuses would have read 3/3 OK and
     *   opened the next rung.
     *
     *   IT MUST BE ONE OF OURS. The strike happens in the parse arms, so only
     *   a reading we can actually read counts.
     *
     * "Answered" has to mean answered, or the sequence gate above it is
     * counting the wrong thing again. */
    r2_battery_t b;
    r2_head_t    h;
    r2_version_t v;
    if (r2_ops_parse_battery(&r, &b) == R2_OPS_OK) {
        r2_telemetry_battery(&s_tm, b.centivolts, now_ms());
        if (is_response) (void)ledger_note(r.seq);
    } else if (r2_ops_parse_head(&r, &h) == R2_OPS_OK) {
        r2_telemetry_dome(&s_tm, h.degrees, now_ms());
        if (is_response) (void)ledger_note(r.seq);
    } else if (r2_ops_parse_version(&r, &v) == R2_OPS_OK) {
        r2_telemetry_version(&s_tm, v.major, v.minor, v.revision, now_ms());
        if (is_response) (void)ledger_note(r.seq);
    }
}

/* Talks to R2. Never touches LVGL. */
/* Defined with the other senders, below: the link loop calls it the moment
 * the operator taps a rung. */
static unsigned run_op(int tier, panel_op_t op);

/* The link loop's tick. Short enough that a tap on the ladder reaches the
 * radio promptly; the keepalive and the polls count beats of it rather than
 * sleeping, so their periods are what they always were. */
#define LINK_TICK_MS 100u
#define LINK_TICKS_PER_BEAT (PANEL_KEEPALIVE_MS / LINK_TICK_MS)
_Static_assert(PANEL_KEEPALIVE_MS >= LINK_TICK_MS &&
               PANEL_KEEPALIVE_MS % LINK_TICK_MS == 0,
               "the keepalive must be a whole number of link ticks: a shorter "
               "one makes the beat `tick % 0`, and a ragged one drifts");

/* THE CONTROLLER GETS ITS OWN TASK, AND ITS OWN RATE.
 *
 * It was polled once per UI tick -- every 40 ms -- and a flick is 40-80 ms of
 * contact, so a flick got one or two looks at the finger. Measured 2026-09-14
 * on an operator's own gestures: of 22 presses, the eight seen exactly ONCE
 * were all misread as taps, three of them opening something nobody asked for.
 * The UI's frame rate has nothing to do with how fast a finger moves, and
 * tying the two together is what made swipes feel random.
 *
 * ONE OWNER. panel_touch_poll accumulates a press across calls, so polling it
 * from here AND from ui_task would race two tasks over s_pressing and the
 * sample count -- the sampling bug's own shape, one layer up. ui_task no
 * longer polls; it only takes the finished gesture.
 *
 * Safe off the display lock by construction: poll() does I2C and touches no
 * LVGL, which is exactly why it was split from panel_touch_render(). */
#define PANEL_TOUCH_TICK_MS 10u

static void touch_task(void *arg)
{
    (void)arg;
    while (1) {
        panel_touch_poll();
        vTaskDelay(pdMS_TO_TICKS(PANEL_TOUCH_TICK_MS));
    }
}

static void link_task(void *arg)
{
    (void)arg;
    /* UNSIGNED, and the beat counts LINK-UP periods, not wall clock. Both are
     * the old loop's behaviour restored rather than preserved by accident: it
     * incremented after the link check, so a period spent disconnected cost
     * nothing and the first poll landed on the first beat of a connection.
     * Counting wall clock instead delayed the battery to t=15 s on every cold
     * link-up and let a flapping link swallow a dome read entirely. Signed
     * would also, at the wrap, make `beat % 10 == 3` unsatisfiable forever --
     * six years out, and a dome that never polls again. */
    unsigned tick = 0;
    unsigned beat = 0;
    while (1) {
        /* A 100 ms TICK, with the keepalive derived from it rather than from
         * the sleep. The loop used to sleep the whole keepalive period, which
         * made it the wrong place to notice a tap -- and noticing taps in the
         * UI task instead meant two tasks sharing the sequence counter and the
         * request tally, the very number this project uses to chase lost
         * replies. One owner, one tick. */
        vTaskDelay(pdMS_TO_TICKS(LINK_TICK_MS));
        tick++;

        /* THE OPERATOR'S TEST FIRST, and before the link check: a tap while he
         * is away must still settle, as NO REPLY rather than as silence. */
        unsigned probe_gen = 0;
        /* INITIALISED TO A VALUE run_op REFUSES. panel_ui writes this under
         * the same lock that hands over the tier and only when there is a
         * request, so this value is never sent -- and if that ever stopped
         * being true, the failure is a refusal and a loud log line rather than
         * whichever op happened to be first in an enum. */
        panel_op_t probe_op = PANEL_OP__COUNT;
        const int probe_tier = panel_ui_take_probe_request(&probe_gen, &probe_op);
        if (probe_tier >= 0) {
            const uint32_t at = now_ms();      /* stamped before the ops go */
            /* Carried through the send, so a panel that left the interior
             * while these were in flight can disown the result rather than
             * start a test against a rung that is gone. */
            /* SENT FIRST, THEN ASKED. A tap with him away fails every op and
             * must read LINK LOST rather than NO REPLY -- our silence, not
             * his. Reading the link before the sends left a window, however
             * small, in which it dropped in between and the verdict blamed
             * him anyway: the answer is about the link the sends actually
             * met. Argument order is unsequenced in C, so the two are
             * separate statements rather than one call. */
            const unsigned sent = run_op(probe_tier, probe_op);
            const bool up = r2_link_is_up();
            /* THE HOLD FIRST, stamped after the sends so it errs long. Before
             * panel_ui_probe_sent, which clears in_flight -- see panel_ui.h. */
            panel_ui_motion_sent(probe_tier, sent, now_ms());
            panel_ui_probe_sent(sent, at, probe_gen, up);
        }

        /* THE STOP, BEFORE THE KEEPALIVE GUARD BELOW. Everything after this
         * point is skipped when the link is down -- which is exactly the
         * moment a stop must still be attempted and its failure reported,
         * rather than the button sitting silent. */
        /* WAKE / GOODNIGHT (E2E v0 slice 1, D-023). A toggle, resolved HERE
         * against what the link actually wants. GOODNIGHT marks us released
         * BEFORE letting go, so the disconnect that follows is not counted as
         * an attempt to reach him; WAKE clears it first so the attempt clock
         * starts at the wake, not at the goodnight an hour ago. */
        if (panel_ui_take_power_request()) {
            if (r2_link_wanted()) {
                r2_telemetry_released(&s_tm, true, now_ms());
                r2_link_release();
                ESP_LOGW(TAG, "GOODNIGHT: keepalive stopped, letting go of him");
            } else {
                r2_telemetry_released(&s_tm, false, now_ms());
                r2_link_wake();
                ESP_LOGW(TAG, "WAKE: looking for him");
            }
        }

        if (panel_ui_take_stop_request()) {
            const r2_stop_report_t st =
                r2_ops_stop_all(next_seq, r2_link_send, NULL);
            ESP_LOGW(TAG, "STOP: %u of 3 away (anim=%d audio=%d legs=%d)",
                     st.sent, (int)st.animation, (int)st.audio, (int)st.legs);
            /* STAMPED HERE, where the halts went -- panel_ui_stop_sent
             * runs on this task and must not read ui_task's clock. */
            panel_ui_stop_sent(st.sent, r2_link_is_up(), now_ms());
        }

        /* Not wanted means no keepalive even while the teardown is still in
         * flight: one more beat would be one more wake command. */
        if (!r2_link_is_up() || !r2_link_wanted()) continue;
        if (tick % LINK_TICKS_PER_BEAT != 0) continue;

        /* Keepalive. This is also what stops him sleeping, which is a real
         * cost and a deliberate one for now: D-023 records that powering him
         * down is us stopping, and the panel's RELEASE control is the place
         * that gets decided -- not here. */
        r2_gate_send(0x13, 0x0D, next_seq(), NULL, 0, r2_link_send, NULL);

        /* HIS FIRMWARE VERSION, once per connection. It is a read-tier probe
         * and the telemetry has always had a slot for it; nothing ever asked,
         * so the R2 LINK interior's R2 FW row could never fill -- honest, and
         * permanently blank. Seen on the glass, 2026-09-14. */
        /* LATCHED ON SUCCESS, NOT ON THE ATTEMPT. This is the only one-shot
         * request in this loop -- battery and dome repeat, so a lost write
         * heals itself on the next tick. A probe latched on the attempt would
         * leave R2 FW blank for the whole connection after one dropped GATT
         * write, with ANSWERED short by one and nothing able to close it. */
        if (!s_version_asked && r2_ops_probe_version(next_seq(), r2_link_send, NULL) > 0) {
            r2_telemetry_note_request(&s_tm);
            s_version_asked = true;
        }

        /* One beat per keepalive period WITH THE LINK UP. Counted here so a
         * beat is never spent on a connection that was not there. */
        const unsigned b = beat++;

        if (b % 5 == 0) {
            r2_telemetry_note_request(&s_tm);
            r2_ops_request_battery(next_seq(), r2_link_send, NULL);
        }
        /* The dome, every 30 s. The face has a DOME field and had no source
         * for it, so it read "---" on a healthy link -- which is the honest
         * rendering of a value nobody asked for, and a field nobody asks for
         * is a field that should not be on the screen. Asking is the cheaper
         * fix. READ ONLY: this asks where he is looking, it does not turn him,
         * and the gate ceiling stays at 'read'. */
        if (b % 10 == 3) {
            r2_telemetry_note_request(&s_tm);
            r2_ops_request_head(next_seq(), r2_link_send, NULL);
        }
        /* The ratio, logged as well as shown. The first run on glass read
         * "35/55 answered" where link_check -- the same stack with no display
         * -- ran 138/138. Either the display is costing us responses or the
         * accounting is wrong, and a number on a panel nobody can screenshot
         * mid-run cannot tell me which. */
        if (b % 20 == 0) {
            uint32_t sent, dropped, admitted, refused;
            r2_link_stats(&sent, &dropped);
            r2_gate_stats(&admitted, &refused);
            ESP_LOGI(TAG, "t+%us  answered %u/%u  | gate adm=%u ref=%u  link sent=%u drop=%u",
                     (unsigned)(now_ms() / 1000), (unsigned)s_tm.responses,
                     (unsigned)s_tm.requests, (unsigned)admitted, (unsigned)refused,
                     (unsigned)sent, (unsigned)dropped);
        }
    }
}

/* RUN A TIER'S TEST. The only tier this firmware can run is READ, because the
 * gate's ceiling is never raised -- and READ is three questions that cannot
 * move him: his battery, his dome's position, his firmware version.
 *
 * Every op goes through r2_gate_send like everything else, so a rung above
 * the ceiling would be refused here even if the ladder offered it. The count
 * returned is how many the gate admitted AND the link took; the panel needs
 * that number to know what a pass looks like. */
static unsigned run_op(int tier, panel_op_t op)
{
    /* A MARKER, NOT A CONTROL -- everything it refuses is refused again one
     * line down, so deleting it changes no behaviour today. It earns its place
     * when the ceiling is raised and that next line is relaxed: whoever does
     * that finds the bundle rule already written at the point of send. Said in
     * one sentence because an earlier draft argued for fifteen lines that it
     * was live protection, which is the claim this repo ranks worst. */
    if (op == PANEL_OP_ALL && !panel_service_tier_may_bundle(tier)) {
        ESP_LOGE(TAG, "REFUSED: tier %d drives an actuator and may not bundle", tier);
        return 0;
    }

    if (tier != (int)R2_TIER_READ) {
        /* Not reachable from the ladder, which only offers what the gate
         * would admit. Said out loud rather than assumed. */
        ESP_LOGE(TAG, "REFUSED: rung %d is not READ, and this build runs only READ",
                 tier);
        return 0;
    }
    /* A FRESH LEDGER PER TEST. Anything outstanding from a previous run is
     * this run's noise -- counting a late reply to the last test as an answer
     * to this one is the same overcounting by another route. */
    portENTER_CRITICAL(&s_ledger_mux);
    panel_ledger_reset(&s_ledger);
    portEXIT_CRITICAL(&s_ledger_mux);

    /* WHAT THIS TAP SENDS, AND IT IS WHAT THE OPERATOR NAMED (#168 part 2).
     * The bundle is one row among four now, not the only thing a rung can do;
     * every other row is exactly one op, chosen by its own name on its own
     * tap. That is what "individually opt-in" asks for, and it is the half the
     * budget rule could not supply -- a cap on how many unnamed things fire is
     * rationing, not consent.
     *
     * WHERE THIS IS, AND WHAT IT IS NOT. An earlier version of this comment
     * called this function "the line every op crosses". It is not: this file
     * emits ops from six places, and the line every op really crosses is
     * r2_gate_send. This is the line every TEST crosses, which is the right
     * place for a rule about what one tap may fire and the wrong place to
     * claim universality. */
    /* THE BUNDLE'S SIZE COMES FROM THE CATALOGUE, NOT FROM A LITERAL HERE.
     * A hardcoded 3 is a second copy of READ_OPS_N with nothing tying them:
     * drop one of READ's questions and the row renders RUN ALL 2 while this
     * fires three, `sent` matches `expected`, and the operator gets a green
     * 3/3 on a row they consented to twice. panel_service is tested; this
     * switch is not, which is exactly why the number must not live here. */
    panel_tier_op_t rows[PANEL_TIER_OPS_MAX];
    const int n_rows = panel_service_tier_ops(tier, rows);
    if (n_rows <= 0) {
        ESP_LOGE(TAG, "REFUSED: tier %d has no op catalogue", tier);
        return 0;
    }
    unsigned all_sends = 0;
    for (int i = 0; i < n_rows; i++)
        if (rows[i].op == PANEL_OP_ALL) all_sends = rows[i].sends;

    unsigned first, budget;
    switch (op) {
    case PANEL_OP_ALL:
        if (all_sends == 0u) {
            /* Asked for a bundle on a list that has no bundle row. */
            ESP_LOGE(TAG, "REFUSED: tier %d offers no bundle", tier);
            return 0;
        }
        first = 0u; budget = all_sends; break;
    case PANEL_OP_BATTERY: first = 0u; budget = 1u; break;
    case PANEL_OP_HEAD:    first = 1u; budget = 1u; break;
    case PANEL_OP_VERSION: first = 2u; budget = 1u; break;
    default:
        /* NOT A FALLTHROUGH TO SOMETHING PLAUSIBLE. An op this function does
         * not recognise is one the catalogue and this switch disagree about,
         * and quietly running the last case would send a command nobody
         * chose. Refused, and loudly, because that disagreement is a bug. */
        ESP_LOGE(TAG, "REFUSED: op %d is not one this build can send", (int)op);
        return 0;
    }

    /* THE SEQ IS TAKEN ONCE AND USED TWICE: sent on the wire and written down
     * here. Calling next_seq() again for the ledger would record a number
     * nothing will ever answer, and the test could never pass. */
    unsigned sent = 0;
    for (unsigned k = first; k < first + budget; k++) {
        const uint8_t sq = next_seq();
        /* WRITTEN DOWN BEFORE IT GOES OUT. r2_link_send hands the frame to the
         * stack and returns; the reply lands on the NimBLE host task. Record
         * after sending and a preemption in that gap drops the strike, so a
         * healthy test settles PARTIAL. An entry for an op that then fails to
         * send is harmless -- `sent` is what becomes `expected`, so an
         * un-strikeable slot is never waited on. */
        portENTER_CRITICAL(&s_ledger_mux);
        panel_ledger_add(&s_ledger, sq);
        portEXIT_CRITICAL(&s_ledger_mux);
        int ok;
        switch (k) {
        case 0:  ok = r2_ops_request_battery(sq, r2_link_send, NULL); break;
        case 1:  ok = r2_ops_request_head(sq, r2_link_send, NULL);    break;
        case 2:  ok = r2_ops_probe_version(sq, r2_link_send, NULL);   break;
        default:
            /* NOT A FALLTHROUGH TO THE LAST OP, for the same reason the op
             * switch above refuses an unknown op. The static assert ties the
             * enum to the table but not to THIS switch: add a fourth READ op
             * to both and every assert still passes, while the BUNDLE would
             * send battery, head, version, version -- `sent` matching
             * `expected`, a green 4/4, and the new op never leaving. Nothing
             * on the glass or in the log would look different.
             *
             * AND THE TEST STILL PASSES, which is worth saying plainly
             * because the first version of this comment claimed it settles
             * PARTIAL. It does not: `ok = 0` means `sent` is not incremented,
             * `sent` becomes `expected`, and three answers to three requests
             * is a pass. The row would read a green 3/3 OK under a label
             * saying RUN ALL 4.
             *
             * The signal is this log line and that mismatch, not a failed
             * verdict -- and the comment fifteen lines above already says why
             * (an entry for an op that fails to send is harmless, because
             * `sent` is what becomes `expected`). Two comments in one function
             * disagreeing is how a reader ends up trusting the wrong one. */
            ESP_LOGE(TAG, "REFUSED: no op is wired at index %u -- the catalogue "
                          "and this switch disagree", k);
            ok = 0;
            break;
        }
        if (ok <= 0) continue;          /* the gate refused it: never asked */
        sent++;
    }
    for (unsigned i = 0; i < sent; i++) r2_telemetry_note_request(&s_tm);
    ESP_LOGW(TAG, "tier %d op %d: %u of %u away", tier, (int)op, sent, budget);
    return sent;
}

/* Injected into panel_ui so the UI file stays free of the BSP. */
static void set_brightness_pct(int percent) { bsp_display_brightness_set(percent); }

/* Draws. Never touches the radio. The two never share anything but the
 * telemetry struct, which is written on the NimBLE host task and read here --
 * every field is word-sized or smaller and a torn read shows one stale value
 * for one frame, which is a redraw away from correct. */
static void ui_task(void *arg)
{
    (void)arg;
    int ticks = 0;
    while (1) {
        /* The I2C read happens OUTSIDE the display lock (review of #150):
         * holding the LVGL lock across a blocking bus transaction lets a
         * wedged controller stall every redraw and every other task that
         * needs the display. */
        if (bsp_display_lock(100)) {
            panel_touch_render();

            /* THE SWIPE NOW REACHES SOMETHING. It was inert when #150 merged
             * and the PR said so in those words, because there was nowhere to
             * swipe to. There are three pages now, so this is the call site
             * that stops it being dead code -- and it was one line, which is
             * exactly the point CLAUDE.md makes about wiring one path before
             * building the layer above. */
            const panel_swipe_t swiped = panel_touch_take_swipe();
            const bool touched = panel_touch_take_activity();
            const bool pressed = panel_touch_take_press();
            if (pressed) panel_ui_note_press();
            int16_t tap_x = 0, tap_y = 0;
            bool tapped = panel_touch_take_tap(&tap_x, &tap_y);

            /* WHILE THE WAKE FRAME IS UP, NO TOUCH REACHES THE PAGES. D-017's
             * point is that a glance must not be able to arm anything, and a
             * swipe completing under the frame is a thumb acting on a screen
             * its owner cannot see.
             *
             * Only a press that LANDS while it is up dismisses it. A finger
             * already on the glass when it rose -- someone mid-swipe on
             * SERVICE -- would otherwise dismiss a frame nobody saw, and its
             * release would then move the page. The dismissing press is voided
             * too, so lifting it does nothing either. */
            panel_swipe_t sw = swiped;
            if (panel_ui_wake_showing()) {
                sw = PANEL_SWIPE_NONE;
                tapped = false;
                if (pressed) {
                    panel_ui_wake_dismiss();
                    panel_touch_void_gesture();
                    ESP_LOGI(TAG, "wake frame dismissed by touch");
                }
            }
            /* Where a gesture goes -- which page, whether it is BACK inside a
             * SERVICE interior, which row a tap hit -- is the renderer's to
             * decide; this loop only reports that one happened. */
            if (sw != PANEL_SWIPE_NONE)
                panel_ui_swipe(sw == PANEL_SWIPE_LEFT ? 1 : -1);
            if (tapped)
                panel_ui_tap(tap_x, tap_y);

            /* The press IN PROGRESS, for WAKE / GOODNIGHT's hold. A completed
             * hold voids the gesture so lifting the finger does nothing more. */
            {
                int16_t hx = 0, hy = 0;
                int32_t hdev = 0;
                bool hvoid = false;
                const bool down = panel_touch_down(&hx, &hy, &hdev, &hvoid);
                /* A press that dismissed the wake frame above is voided, and
                 * must stay a dismissal: D-017, a glance arms nothing. */
                if (panel_ui_hold(down, hx, hy, hvoid, hdev, now_ms()))
                    panel_touch_void_gesture();
            }

            /* A HUMAN TOUCHING IT COUNTS AS ACTIVITY. Without this the dim
             * timer keys only on the DATA changing, so someone who picks up
             * the droid and taps is reading a 40% screen because the battery
             * happened to report the same voltage as a minute ago. */
            const bool changed = panel_ui_update(&s_tm, now_ms());
            panel_ui_burn_in(now_ms(), changed || touched || swiped != PANEL_SWIPE_NONE,
                             set_brightness_pct);
            bsp_display_unlock();
        }


        /* P1 progress, once a minute (#101). Without this the panel records
         * touch extremes and never says so, which makes the measurement
         * INVISIBLE -- and an operator who has done the corners has no way to
         * know whether it worked. It also distinguishes "nobody touched it"
         * from "touch is not wired", which look identical from here.
         *
         * This block was lost once already: the edit that added it targeted an
         * anchor a previous edit had changed, the replace silently did nothing,
         * and only `int ticks = 0;` survived -- set, never read, and not loud
         * enough to fail the build. */
        if (++ticks % 1500 == 0) {
            panel_touch_extremes_t ex;
            panel_touch_extremes(&ex);
            if (ex.points == 0) {
                ESP_LOGI(TAG, "P1: no touch points yet");
            } else {
                ESP_LOGI(TAG, "P1: %u points  x %d..%d  y %d..%d",
                         (unsigned)ex.points, ex.min_x, ex.max_x,
                         ex.min_y, ex.max_y);
                ESP_LOGI(TAG, "    edge gaps: L%d R%d T%d B%d  (0 = bezel reached)",
                         ex.min_x, 367 - ex.max_x, ex.min_y, 447 - ex.max_y);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(40));
    }
}

/* Screenshots get their OWN task, with a generous stack.
 *
 * They started life inside the UI task and overflowed its 4 KB in about a
 * minute: lv_snapshot_take plus the partition writes are far heavier than a
 * redraw, and the crash-loop was INVISIBLE in the thing I was using to check
 * the panel -- the captured frame looked perfect every time, because a frame
 * captured two seconds before a reboot looks exactly like a healthy one. Only
 * the serial log showed five boots in four minutes.
 *
 * So the diagnostic is isolated from the thing it diagnoses: a screenshot can
 * now fail, or run out of stack, without taking the panel down with it. */
/* The periodic capture, replaced wholesale by the tour when that is built:
 * two tasks writing slot 0 would race, and only one of them is wanted. */
#ifndef PANEL_SHOT_TOUR
static void shot_task(void *arg)
{
    (void)arg;
#ifdef PANEL_SHOT_WAKE
    /* The wake frame lasts six seconds and the periodic capture runs once a
     * minute, so the periodic shot would catch it one time in ten. This
     * waits for it to rise and captures it 700 ms in, after the 450 ms
     * sweep has cleared the screen. It never shoots again until the frame
     * has gone and come back, so the captured frame is not overwritten by
     * the resting face that follows. Reading the flag from another task is
     * a benign race for a build that exists only to take this picture. */
    bool was_up = false;
    while (1) {
        const bool up = panel_ui_wake_showing();
        if (up && !was_up) {
            vTaskDelay(pdMS_TO_TICKS(700));
            panel_shot_take();
            ESP_LOGW(TAG, "shot: wake frame captured");
        }
        was_up = up;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
#else
    vTaskDelay(pdMS_TO_TICKS(15000));   /* let the link settle first */
    while (1) {
        panel_shot_take();
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
#endif
}

#endif  /* PANEL_SHOT_TOUR */

#ifdef PANEL_SHOT_TOUR
/* THE TOUR: one boot, nine frames, no finger.
 *
 * Every visual check of this panel has cost the operator a walk to the droid
 * and a phone photo, and the UI now has nine views -- the face, the menu and
 * seven interiors. This build drives itself through them and captures each
 * into its own slot, so one flash and one esptool read shows the lot.
 *
 * IT TAPS RATHER THAN CALLING THE RENDERER. Rows are opened by scrolling
 * them into view and tapping where they landed, through panel_ui_tap -- so
 * the pictures also exercise the hit test against real, scrolled coordinates,
 * which no host test can reach.
 *
 * WHAT IT DOES NOT PROVE, and the distinction matters: no finger is involved.
 * It enters below panel_touch.c, so the controller, the press and release
 * edges, the tap-versus-scroll thresholds and the moving-list guard are all
 * untouched by this. It proves the hit-test geometry and what each view draws;
 * it says nothing about touch. */
static bool s_tour_ok = true;

/* Take the display lock or say why not. A silent failure here would capture
 * whatever was on screen and still print TOUR COMPLETE. */
static bool tour_lock(const char *step)
{
    if (bsp_display_lock(1000)) return true;
    ESP_LOGE(TAG, "TOUR ABORTED: could not take the display lock at %s", step);
    s_tour_ok = false;
    return false;
}

/* CHECK THE GLASS, SHOOT, CHECK AGAIN. The panel is live while the tour runs:
 * on a bench with no droid it reaches OFFLINE and the wake frame fires, which
 * brings STATUS forward -- landing, if the timing is unlucky, between opening
 * an interior and photographing it. That would file the face under an
 * interior's name and still print TOUR COMPLETE.
 *
 * So each capture asks what is showing, puts it back once if something moved
 * it, and leaves the slot EMPTY rather than photograph the wrong view. The
 * check before is not enough on its own: the snapshot takes the display lock
 * again, so the view could move in between. Hence the second check after,
 * which ERASES the slot it just wrote. A missing picture is a finding; a
 * wrong one under a right name is the lie this rig exists to prevent. */
static void tour_shot(unsigned slot, const char *what, int want)
{
    /* EMPTY BY CONSTRUCTION, BEFORE ANYTHING ELSE. Every exit below used to
     * leave whatever the slot already held, which was harmless while each slot
     * got exactly one shot -- and wrong the moment slot 4 took the ladder and
     * then the op list over it. Three separate early returns (a lock timeout,
     * a recheck lock timeout, a failed write) each left the LADDER in a slot
     * grab_tour.sh files as hw-test-ops.png, and one of them says in a comment
     * that an empty slot is the honest outcome.
     *
     * Erasing here rather than fixing four branches means the invariant is
     * structural -- PROVIDED the erase worked. It returns whether it did, and
     * an earlier version discarded that under the words "whatever happens
     * next": on a failed erase the slot keeps the previous picture and every
     * "slot left EMPTY" below becomes the lie this rig exists to prevent. */
    if (!panel_shot_erase_slot(slot)) {
        ESP_LOGE(TAG, "TOUR %u/%u: could not clear the slot for %s -- it may "
                      "still hold an older frame, so nothing is captured",
                 slot + 1, (unsigned)PANEL_SHOT_SLOTS, what);
        s_tour_ok = false;
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(400));      /* let LVGL draw it */

    if (!tour_lock(what)) return;      /* the slot is already erased above */
    if (!panel_ui_debug_showing(want)) {
        ESP_LOGW(TAG, "TOUR: %s was interrupted -- restoring it", what);
        panel_ui_debug_restore(want);
    }
    lv_refr_now(NULL);
    const bool ready = panel_ui_debug_showing(want);
    bsp_display_unlock();

    if (!ready) {
        /* ERASED, NOT MERELY SKIPPED -- and "slot left EMPTY" was a lie the
         * moment a slot could be written twice. Returning here was correct
         * while every slot got exactly one shot; slot 4 now takes the ladder
         * first and the op list over it, so a failed op-list capture left the
         * LADDER in a slot grab_tour.sh files as hw-test-ops.png. The right
         * filename over the wrong picture, which is the failure this rig
         * exists to prevent and which this file names three times.
         *
         * It bites on the bench run specifically: with no droid every op
         * settles NO REPLY, s_op_passed stays 0, and this is the branch that
         * takes. The erase added in the caller sits on a branch that case does
         * not reach. */
        ESP_LOGE(TAG, "TOUR %u/%u: %s is NOT on the glass -- slot left EMPTY",
                 slot + 1, (unsigned)PANEL_SHOT_SLOTS, what);
        s_tour_ok = false;
        return;
    }

    if (panel_shot_take_slot(slot)) {
        /* Still the same view? The snapshot took the lock separately, so a
         * state change could have moved the panel under it. */
        if (!tour_lock("recheck")) return;
        const bool still = panel_ui_debug_showing(want);
        bsp_display_unlock();
        if (!still) {
            /* ERASE FIRST, THEN SAY SO. The old line announced "slot erased"
             * above a call whose answer it threw away, so a failed erase
             * published the mid-capture frame under the right filename while
             * the log said the slot was empty -- the rig's own failure mode,
             * asserted in its own words. */
            const bool cleared = panel_shot_erase_slot(slot);
            if (cleared) {
                ESP_LOGE(TAG, "TOUR %u/%u: %s moved DURING the capture -- slot "
                              "erased", slot + 1, (unsigned)PANEL_SHOT_SLOTS,
                         what);
            } else {
                ESP_LOGE(TAG, "TOUR %u/%u: %s moved DURING the capture AND the "
                              "slot could not be erased -- it still holds a "
                              "frame, do NOT trust this picture",
                         slot + 1, (unsigned)PANEL_SHOT_SLOTS, what);
            }
            s_tour_ok = false;
            return;
        }
        ESP_LOGW(TAG, "TOUR %u/%u: %s", slot + 1, (unsigned)PANEL_SHOT_SLOTS, what);
    } else {
        /* An empty slot is the honest outcome, and grab_tour.sh prints NO
         * FRAME for it. Silence here would have been "TOUR COMPLETE" beside
         * a missing picture. */
        ESP_LOGE(TAG, "TOUR %u/%u: %s NOT CAPTURED", slot + 1,
                 (unsigned)PANEL_SHOT_SLOTS, what);
        s_tour_ok = false;
    }
}

/* Which SERVICE row each interior sits on, and what to call its picture. */
static const int k_tour_row[] = { 1, 2, 3, 4, 5, 6, 7 };
static const char *const k_tour_name[] = {
    "R2 LINK", "DIAGNOSTICS", "HARDWARE TEST", "PROVISIONING",
    "VOICE", "CAMERA", "ABOUT",
};

static void tour_task(void *arg)
{
    (void)arg;
    /* THE TOUR IS LONGER THAN IT WAS. Walking every op row adds three more
     * PANEL_PROBE_TIMEOUT_MS + 600 waits on top of the one the ladder's
     * single run already cost, and a refused-tap step after them. MEASURED
     * END TO END ON THE BOARD, first frame to TOUR COMPLETE: 18.3 s -> 47.1 s,
     * so 28.8 s of tour inside a 60 s step.
     *
     * RE-DERIVED TWICE, and the second time caught this sentence already stale
     * again: the figure was written as 24.8 s and the refused-tap step added
     * four seconds in the same edit. A duration is the easiest claim in a
     * comment to leave behind, because nothing fails when it does.
     *
     * AND IT IS NOW RED ON A BENCH WITH NO DROID. Every op settles NO REPLY,
     * s_op_passed stays 0, and slot 4 is left empty by design. That is honest
     * and it collapses "no droid present" with "rendering broken" into one
     * verdict -- the rig's old green-on-a-bench property is gone, deliberately,
     * because a picture of an op list with no verdicts proves nothing about
     * the thing this slice changed. */
    /* Long enough for the link to settle: a STATUS frame taken before that is
     * a picture of WAKING, which is a real state but not the resting one. It
     * is also past the wake frame that fires ~5 s in when no droid answers.
     * The whole tour finishes well inside the 60 s burn-in drift step, so the
     * nine frames share one alignment. */
    vTaskDelay(pdMS_TO_TICKS(12000));

    /* Blank every slot first, so a tour that dies half way leaves EMPTY slots
     * rather than the tail of the last one, which would decode perfectly.
     *
     * A FAILURE HERE IS NOT FATAL, and the reason is worth stating so nobody
     * "fixes" it into a return. Every slot a tour_shot reaches is erased again
     * at the top of tour_shot, which checks its own answer; the branches that
     * never reach a tour_shot now erase and check for themselves. So the tour
     * remains honest slot by slot, and this flag is what keeps the run's
     * overall verdict from reading clean. */
    if (!panel_shot_erase_all()) {
        ESP_LOGE(TAG, "TOUR: could not blank the slots up front -- every "
                      "picture below is still erased and checked on its own "
                      "path, but the run is not clean");
        s_tour_ok = false;
    }

    if (!tour_lock("STATUS")) vTaskDelete(NULL);
    panel_ui_show_page(0);
    bsp_display_unlock();
    tour_shot(0, "STATUS face", PANEL_TOUR_STATUS);

    if (!tour_lock("SERVICE")) vTaskDelete(NULL);
    panel_ui_show_page(1);
    bsp_display_unlock();
    tour_shot(1, "SERVICE menu", PANEL_TOUR_MENU);

    for (unsigned i = 0; i < sizeof k_tour_row / sizeof k_tour_row[0]; i++) {
        bool opened = false;
        if (!tour_lock(k_tour_name[i])) break;
        /* FROM THE MENU, EVERY TIME. The tap below is routed to whatever is
         * already open, so starting from an interior made the result depend on
         * what that interior does with a tap at a menu row's coordinates. */
        panel_ui_debug_to_menu();
        /* Scrolls the row into view, taps where it actually landed, and says
         * whether that opened the row asked for. */
        opened = panel_ui_debug_open_row(k_tour_row[i]);
        lv_refr_now(NULL);
        bsp_display_unlock();

        if (opened) {
            tour_shot(2 + i, k_tour_name[i], k_tour_row[i]);

            /* THE LADDER GETS RUN, NOT JUST LISTED, AND IT NOW TAKES TWO
             * TAPS. A rung opens its ops; an op row runs one (#168 part 2).
             * The slot is re-captured at the end so the picture shows the op
             * list carrying a verdict, which is the view an operator actually
             * arms something from -- a ladder that draws RUN proves nothing
             * about what tapping it does, and now it does not even send.
             *
             * ONE SLOT, TWO VIEWS, and the partition has no tenth: 9 x 0x52000
             * is 2.88 MiB of a 3 MiB partition and a tenth would need 3.20.
             * The ladder loses, because it is
             * the view that did not change and the op list is the one nobody
             * has ever seen. */
            if (k_tour_row[i] == 3) {          /* HARDWARE TEST */
                bool asked = false;
                if (tour_lock("open rung")) {
                    /* READ. False when the rung refused -- a tier with no
                     * catalogue opens nothing, and photographing the ladder
                     * under the op list's name is the lie this rig exists to
                     * prevent. */
                    asked = panel_ui_debug_open_rung(0);
                    bsp_display_unlock();
                }
                if (!asked) {
                    ESP_LOGE(TAG, "TOUR: rung 0 did not open its ops");
                    s_tour_ok = false;
                }

                /* EVERY ROW, NOT JUST THE BUNDLE. Row 0 fires three ops on one
                 * tap, which is exactly what the ladder did before this slice
                 * -- so a tour that ran only row 0 would exercise none of the
                 * new path and report green. The single-op rows are the code
                 * that changed; they are the ones that have to go out over a
                 * real radio to a real droid.
                 *
                 * READ MOVES NOTHING, which is what makes this affordable. A
                 * tour of a tier that moved him could not walk its rows
                 * unattended, and should not try. */
                bool rows_ok = asked;
                for (int r = 0; rows_ok && r < panel_ui_debug_op_count(); r++) {
                    bool fired = false;
                    if (tour_lock("run op")) {
                        fired = panel_ui_debug_run_op(r);
                        bsp_display_unlock();
                    }
                    if (!fired) {
                        ESP_LOGE(TAG, "TOUR: op row %d refused the tap", r);
                        s_tour_ok = false;
                        rows_ok = false;
                        break;
                    }
                    /* Past the probe's own timeout, so the next tap is not
                     * refused by the one-at-a-time guard and the frame shows a
                     * settled verdict rather than the "..." in between. */
                    vTaskDelay(pdMS_TO_TICKS(PANEL_PROBE_TIMEOUT_MS + 600u));
                    if (!panel_ui_debug_probe_settled()) {
                        /* Photographing a "..." and calling it a result is
                         * the lie this rig exists not to tell. */
                        ESP_LOGE(TAG, "TOUR: op row %d never settled", r);
                        s_tour_ok = false;
                        rows_ok = false;
                        break;
                    }
                    ESP_LOGW(TAG, "TOUR: op row %d %s", r,
                             panel_ui_debug_probe_passed() ? "PASSED"
                                                           : "did NOT pass");
                }
                /* THE REFUSED TAP, AND WHAT THE ROW SAYS AFTERWARDS.
                 *
                 * Nothing exercised this path: panel_ui_debug_run_op returns
                 * false on a refusal and the loop above treats false as an
                 * abort, so a refused tap had never once occurred during a
                 * tour -- which means the green board run PROVED THE BUSY
                 * FLASH NEVER FIRED, rather than proving it works. The bug it
                 * was added to fix (a settled verdict coming back as a green
                 * RUN) would have shipped invisible to every test and every
                 * picture.
                 *
                 * Row 0 has just settled. Tap row 1 to start a test, then tap
                 * row 0 INSIDE its window: refused, so row 0 flashes BUSY over
                 * its own verdict. After the flash expires it must read that
                 * verdict again -- not "RUN", which is what the first version
                 * wrote back. */
                if (rows_ok) {
                    char kept[16] = "";
                    bool ok_flash = false;
                    if (tour_lock("busy flash")) {
                        snprintf(kept, sizeof kept, "%s",
                                 panel_ui_debug_op_says(0));
                        ok_flash = panel_ui_debug_run_op(1);
                        bsp_display_unlock();
                    }
                    if (ok_flash && tour_lock("busy tap")) {
                        ok_flash = panel_ui_debug_tap_op_expect_refusal(0);
                        bsp_display_unlock();
                    }
                    /* KEPT MUST SAY SOMETHING. If row 0 had no label both
                     * sides of the comparison below would be "" and the check
                     * would pass having proved nothing -- the shape this repo
                     * keeps getting bitten by. */
                    if (ok_flash && kept[0] == '\0') {
                        ESP_LOGE(TAG, "TOUR: row 0 said nothing before the "
                                      "flash -- the check would be vacuous");
                        ok_flash = false;
                    }
                    if (!ok_flash) {
                        ESP_LOGE(TAG, "TOUR: could not stage a refused tap");
                        s_tour_ok = false;
                    } else {
                        /* THE FLASH IS OBSERVED, NOT INFERRED. The helper
                         * reports a refusal from the probe counter not moving,
                         * which a tap landing on NOTHING also satisfies -- and
                         * the comparison below would then pass having
                         * exercised no flash at all. Look at the row while the
                         * flash should be up. */
                        vTaskDelay(pdMS_TO_TICKS(200));
                        const char *mid = "";
                        char busy[16] = "";
                        if (tour_lock("busy mid")) {
                            /* COPIED UNDER THE LOCK. lv_label_get_text hands
                             * back the label's own buffer, which ui_task is
                             * free to realloc the moment the lock is dropped. */
                            mid = panel_ui_debug_op_says(0);
                            snprintf(busy, sizeof busy, "%s", mid);
                            bsp_display_unlock();
                        }
                        if (strcmp(busy, "BUSY") != 0) {
                            ESP_LOGE(TAG, "TOUR: row 0 read \"%s\" during the "
                                          "flash, not BUSY -- no flash fired, "
                                          "so the check below proves nothing",
                                     busy);
                            s_tour_ok = false;
                            rows_ok = false;
                        }
                    }
                    if (ok_flash && rows_ok) {
                        /* Past the flash, and past row 1's own window so the
                         * panel is idle again for the shot below. */
                        vTaskDelay(pdMS_TO_TICKS(PANEL_REFUSE_FLASH_MS + 400u));
                        /* COPIED UNDER THE LOCK, like `kept` above it and
                         * unlike the first version of this line: the pointer
                         * lv_label_get_text returns is the LABEL'S OWN buffer,
                         * and ui_task reallocs it on the next repaint and frees
                         * it outright on a view change. Reading it after
                         * bsp_display_unlock was a use-after-unlock with a
                         * narrow window and no symptom. */
                        char now[16] = "";
                        if (tour_lock("busy check")) {
                            snprintf(now, sizeof now, "%s",
                                     panel_ui_debug_op_says(0));
                            bsp_display_unlock();
                        }
                        if (strcmp(now, kept) != 0) {
                            ESP_LOGE(TAG, "TOUR: row 0 said \"%s\" before the "
                                          "flash and \"%s\" after -- the flash "
                                          "ATE THE VERDICT", kept, now);
                            s_tour_ok = false;
                            rows_ok = false;
                        } else {
                            ESP_LOGW(TAG, "TOUR: row 0 kept \"%s\" through a "
                                          "BUSY flash", kept);
                        }
                        vTaskDelay(pdMS_TO_TICKS(PANEL_PROBE_TIMEOUT_MS + 400u));
                    }
                }

                /* ON ROWS_OK, NOT ON s_tour_ok. The tour-wide flag carries
                 * every earlier step's failure, and suppressing THIS picture
                 * because the VOICE interior had a bad moment would lose the
                 * evidence for the thing being changed. */
                if (rows_ok) {
                    tour_shot(2 + i, "HW TEST ops after RUN", PANEL_TOUR_OPS);
                } else {
                    /* THE SLOT IS ERASED, NOT LEFT HOLDING THE LADDER. Both
                     * views share slot 4 and grab_tour.sh files it under one
                     * name; a ladder sitting there when the ops failed to run
                     * would be the right filename over the wrong picture --
                     * the exact failure this rig exists to prevent, relocated
                     * into the naming script. Empty is a finding. */
                    if (panel_shot_erase_slot(2 + i)) {
                        ESP_LOGE(TAG, "TOUR: the ops did not run -- slot %u "
                                      "erased rather than left holding the "
                                      "ladder", 2 + i);
                    } else {
                        /* THE ONE CASE THE COMMENT ABOVE DEPENDS ON. A failed
                         * erase leaves the LADDER in the slot grab_tour.sh
                         * files as hw-test-ops.png -- the right filename over
                         * the wrong picture, which is the whole reason this
                         * branch exists. Say it, and fail the tour. */
                        ESP_LOGE(TAG, "TOUR: the ops did not run AND slot %u "
                                      "could not be erased -- it still holds "
                                      "the LADDER, which grab_tour.sh will "
                                      "file as hw-test-ops. Do NOT trust it.",
                                 2 + i);
                        /* ALREADY false on every path that reaches here --
                         * `asked` failing sets it, and so does every
                         * `rows_ok = false`. Set again so this branch does not
                         * depend on an invariant held two hundred lines away
                         * by four separate assignments. Redundant today, and
                         * said so rather than left reading load-bearing. */
                        s_tour_ok = false;
                    }
                }
            }
        } else {
            /* NO PICTURE AT ALL, AND NOTHING ERASED THIS SLOT ON THIS
             * PATH. tour_shot is what erases per slot, and this branch never
             * reaches it -- so "stays erased" rested entirely on the
             * erase_all at the top of the tour, whose failure only sets
             * s_tour_ok and lets the tour run on. Erase it here and report
             * what actually happened, rather than asserting an emptiness
             * nothing on this path established. */
            if (panel_shot_erase_slot(2 + i)) {
                ESP_LOGE(TAG, "TOUR: tapping row %d did NOT open %s -- slot %u "
                              "left EMPTY", k_tour_row[i], k_tour_name[i],
                         2 + i);
            } else {
                ESP_LOGE(TAG, "TOUR: tapping row %d did NOT open %s AND slot %u "
                              "could not be erased -- it may still hold an "
                              "older frame. Do NOT trust it.",
                         k_tour_row[i], k_tour_name[i], 2 + i);
            }
            s_tour_ok = false;
        }

        if (!tour_lock("back")) break;
        if (opened) panel_ui_swipe(-1);   /* back, the way a finger would */
        /* However this iteration went -- a failed open, a wake frame that
         * pulled STATUS forward mid-capture, a back swipe from a page the
         * interior had already left -- the next tap needs SERVICE. Asserted
         * rather than assumed: every later tap would otherwise bounce off
         * panel_ui_tap's "not on SERVICE" guard, turning one failure into a
         * run of empty slots. */
        if (panel_ui_page() != 1) panel_ui_show_page(1);
        lv_refr_now(NULL);
        bsp_display_unlock();
    }

    if (s_tour_ok) {
        ESP_LOGW(TAG, "TOUR COMPLETE -- read all %u slots with:",
                 (unsigned)PANEL_SHOT_SLOTS);
        ESP_LOGW(TAG, "  tools/grab_tour.sh <port> <outdir> [build dir]");
    } else {
        ESP_LOGE(TAG, "TOUR FINISHED WITH FAILURES -- see the errors above; do "
                      "not trust the slot names");
    }
    vTaskDelete(NULL);
}
#endif

#ifdef PANEL_P2_RECONNECT
/* P2 — time the PANEL'S OWN reconnect (#101, gates AC8).
 *
 * AC8 wants `waking` to show BOUNDED PROGRESS against a real duration, and the
 * epic is explicit that the ~12 s figure floating around is the MAC DAEMON'S
 * and must not calibrate this bar. Nobody has ever timed the board's.
 *
 * Behind a build flag because dropping the link on purpose, repeatedly, is the
 * last thing a resting panel should do. It is safe -- every op here is read
 * tier, and a dropped link is what "default to STOP" already contemplates --
 * but it is not resting behaviour and must not be reachable by accident.
 *
 * n > 1 deliberately. A single reconnect is an anecdote, and a progress bar
 * calibrated to one sample will overrun or stall for every user of it. */
#define P2_TRIALS 8

static void p2_task(void *arg)
{
    (void)arg;
    uint32_t ms[P2_TRIALS];
    int n = 0;

    while (!r2_link_is_up()) vTaskDelay(pdMS_TO_TICKS(200));
    ESP_LOGW(TAG, "P2: timing %d deliberate reconnects", P2_TRIALS);

    while (n < P2_TRIALS) {
        vTaskDelay(pdMS_TO_TICKS(4000));      /* settle between trials */
        if (!r2_link_is_up()) continue;

        const uint32_t t0 = now_ms();
        r2_link_disconnect();
        /* Wait for the link to LEAVE up first. Timing from the request would
         * fold our own teardown into his reconnect and quietly inflate it. */
        while (r2_link_is_up()) vTaskDelay(pdMS_TO_TICKS(5));
        const uint32_t dropped = now_ms();

        while (!r2_link_is_up()) vTaskDelay(pdMS_TO_TICKS(5));
        ms[n] = now_ms() - dropped;
        ESP_LOGW(TAG, "P2 trial %d/%d: %"PRIu32" ms  (teardown %"PRIu32" ms)",
                 n + 1, P2_TRIALS, ms[n], dropped - t0);
        n++;
    }

    uint32_t lo = ms[0], hi = ms[0], sum = 0;
    for (int i = 0; i < n; i++) {
        if (ms[i] < lo) lo = ms[i];
        if (ms[i] > hi) hi = ms[i];
        sum += ms[i];
    }
    ESP_LOGW(TAG, "P2 RESULT: n=%d  min %"PRIu32" ms  max %"PRIu32
                  " ms  mean %"PRIu32" ms", n, lo, hi, sum / (uint32_t)n);
    ESP_LOGW(TAG, "  AC8's bar should be bounded by the MAX, not the mean:");
    ESP_LOGW(TAG, "  a bar that finishes early and waits reads as broken, and");
    ESP_LOGW(TAG, "  one that overruns reads as a hang.");
    vTaskDelete(NULL);
}
#endif

#ifdef PANEL_P4_IDLE
/* P4 — his idle timeout (#101, gates how `released` is presented over time).
 *
 * We never connect. The keepalive IS the wake command, so any session that
 * connects has already destroyed the thing it wanted to measure.
 *
 * WHAT THIS CAN CONCLUDE IS NARROWER THAN IT LOOKS, and saying so up front is
 * the point: the only sleep this project has ever observed was VISUAL -- he
 * reverted to his resting alternation and faded out. Whether a sleeping droid
 * stops advertising has never been established. So:
 *
 *   advertising STOPS   -> a real, machine-readable transition worth timing
 *   advertising CONTINUES -> INSTRUMENT-LIMITED. Not "he stayed awake".
 *
 * The second outcome is the likely one and must not be written up as a
 * finding about the droid. */
static void p4_task(void *arg)
{
    (void)arg;
    /* The flag is set in app_main, BEFORE the host syncs -- setting it here
     * raced the first scan, which could find him and connect in the 500 ms
     * below. That race is why an earlier session recorded this build as
     * unable to produce an offline frame. Set again, harmlessly, so this
     * task still reads as self-contained. */
    r2_link_set_scan_only(true);
    /* RESTART the scan -- now belt and braces rather than the fix it was.
     * The flag is read when ble_gap_disc is called, and it is set in app_main
     * before the host syncs, so on_sync's own scan already has duplicate
     * filtering off. Before that, this restart was the only thing stopping us
     * seeing him ONCE and never again: the first run reported "1 advert this
     * minute", which is the dedup filter rather than his advertising rate. */
    vTaskDelay(pdMS_TO_TICKS(500));
    r2_link_start();
    ESP_LOGW(TAG, "P4: scan-only. We will NOT connect, because the keepalive");
    ESP_LOGW(TAG, "    is his wake command and connecting destroys the thing");
    ESP_LOGW(TAG, "    being measured. Watching the advertisement only.");
    const uint32_t t0 = now_ms();
    uint32_t last_seen = 0, adverts = 0, prev_adverts = 0;
    bool ever_seen = false;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));
        r2_link_adverts(&adverts, &last_seen);
        const uint32_t mins = (now_ms() - t0) / 60000u;
        const uint32_t since = adverts - prev_adverts;
        prev_adverts = adverts;
        if (since > 0) {
            ever_seen = true;
            ESP_LOGW(TAG, "P4 t+%2umin: %u adverts this minute (still advertising)",
                     (unsigned)mins, (unsigned)since);
        } else if (!ever_seen) {
            /* Never seen him AT ALL. That is an instrument failure, not a
             * sleeping droid, and the two are indistinguishable unless this
             * says so -- the first P4 run reported zero adverts because a
             * stale scan was deduplicating him, and it read exactly like
             * sleep. */
            ESP_LOGE(TAG, "P4 t+%2umin: NO ADVERTS EVER SEEN. The scanner has "
                          "not produced a single positive, so silence here is "
                          "an INSTRUMENT FAILURE and proves nothing about him.",
                     (unsigned)mins);
        } else {
            ESP_LOGW(TAG, "P4 t+%2umin: *** NO ADVERTS THIS MINUTE *** "
                          "(last seen t+%umin, after %u total)",
                     (unsigned)mins, (unsigned)((last_seen - t0) / 60000u),
                     (unsigned)adverts);
        }
    }
}
#endif

/* Only if wanted. The panel boots RELEASED (E2E v0 slice 1): it no longer
 * takes R2 the moment the host syncs, which also stops it stealing his one
 * BLE central slot from the Mac. Measurement builds keep the default. */
static void on_sync(void) { if (r2_link_wanted()) r2_link_start(); }
static void host_task(void *p) { (void)p; nimble_port_run(); nimble_port_freertos_deinit(); }

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    bsp_display_start();
    /* Explicitly, every boot. Brightness is a CO5300 register that SURVIVES A
     * REFLASH, so a previous app's low slider leaves a healthy app looking
     * like dead hardware. The display's version of "assert the status on
     * connect, never inherit it". */
    ESP_ERROR_CHECK(bsp_display_brightness_set(panel_ui_full_brightness()));

    if (bsp_display_lock(2000)) {
        panel_ui_create();
        panel_touch_init();
        bsp_display_unlock();
    }
    ESP_LOGI(TAG, "STATUS frame drawn; ceiling is '%s'",
             r2_gate_tier_name(r2_gate_get_ceiling()));

    r2_telemetry_reset(&s_tm);
#if !defined(PANEL_P4_IDLE) && !defined(PANEL_P2_RECONNECT)
    /* BOOT RELEASED. Before the host syncs, so on_sync sees it. Waking him is
     * a deliberate hold on the face, never a side effect of power-on. */
    r2_link_set_wanted(false);
    r2_telemetry_released(&s_tm, true, now_ms());
#endif
#ifdef PANEL_P4_IDLE
    /* BEFORE the host syncs: on_sync starts a scan the moment NimBLE is up,
     * and a scan started without this flag will connect to him. Deciding not
     * to connect after that has already happened is too late. */
    r2_link_set_scan_only(true);
#endif
    const r2_link_cbs_t cbs = { .on_state = on_state, .on_frame = on_frame, .ctx = NULL };
    r2_link_init(&cbs);

    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = on_sync;
    nimble_port_freertos_init(host_task);

    /* Above the link and the UI: a finger is the one input with a human
     * waiting on it, and its work is a 6-byte I2C read that yields on a
     * semaphore. Its per-point logging is ESP_LOGD for the same reason -- at
     * INFO this task would hold a 115200 console for most of a drag, from
     * above both of them. */
    xTaskCreate(touch_task, "panel_touch", 3072, NULL, 5, NULL);
    xTaskCreate(link_task, "r2_link_task", 4096, NULL, 4, NULL);
    xTaskCreate(ui_task,   "panel_ui",     4096, NULL, 3, NULL);
#ifdef PANEL_SHOT_TOUR
    xTaskCreate(tour_task, "panel_tour",   8192, NULL, 2, NULL);
#else
    xTaskCreate(shot_task, "panel_shot",   8192, NULL, 2, NULL);
#endif
#ifdef PANEL_P4_IDLE
    xTaskCreate(p4_task,   "panel_p4",     4096, NULL, 4, NULL);
#endif
#ifdef PANEL_P2_RECONNECT
    xTaskCreate(p2_task,   "panel_p2",     4096, NULL, 4, NULL);
#endif
}
