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
            panel_ui_probe_sent(sent, at, probe_gen, up);
        }

        /* THE STOP, BEFORE THE KEEPALIVE GUARD BELOW. Everything after this
         * point is skipped when the link is down -- which is exactly the
         * moment a stop must still be attempted and its failure reported,
         * rather than the button sitting silent. */
        if (panel_ui_take_stop_request()) {
            const r2_stop_report_t st =
                r2_ops_stop_all(next_seq, r2_link_send, NULL);
            ESP_LOGW(TAG, "STOP: %u of 3 away (anim=%d audio=%d legs=%d)",
                     st.sent, (int)st.animation, (int)st.audio, (int)st.legs);
            /* STAMPED HERE, where the halts went -- panel_ui_stop_sent
             * runs on this task and must not read ui_task's clock. */
            panel_ui_stop_sent(st.sent, r2_link_is_up(), now_ms());
        }

        if (!r2_link_is_up()) continue;
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
    if (tier != (int)R2_TIER_READ) {
        /* Not reachable from the ladder, which only offers what the gate
         * would admit. Said out loud rather than assumed. */
        ESP_LOGE(TAG, "REFUSED: rung %d is not READ, and this build runs only READ",
                 tier);
        return 0;
    }

    /* A BUNDLE IS REFUSED WHERE IT IS NOT LEGAL, and this is now a check with
     * somewhere to fail rather than a marker. panel_service_tier_ops never
     * offers an actuator tier a RUN ALL row, so the operator cannot ask for
     * one -- but "the UI does not offer it" is a claim about a renderer, and
     * the rule belongs at the line that sends. Same shape as the gate: the
     * ladder offers only what the gate would admit AND the gate refuses the
     * rest regardless. */
    if (op == PANEL_OP_ALL && !panel_service_tier_may_bundle(tier)) {
        ESP_LOGE(TAG, "REFUSED: tier %d drives an actuator and may not bundle", tier);
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
    unsigned first, budget;
    switch (op) {
    case PANEL_OP_ALL:     first = 0u; budget = 3u; break;
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
        default: ok = r2_ops_probe_version(sq, r2_link_send, NULL);   break;
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
    vTaskDelay(pdMS_TO_TICKS(400));      /* let LVGL draw it */

    if (!tour_lock(what)) return;
    if (!panel_ui_debug_showing(want)) {
        ESP_LOGW(TAG, "TOUR: %s was interrupted -- restoring it", what);
        panel_ui_debug_restore(want);
    }
    lv_refr_now(NULL);
    const bool ready = panel_ui_debug_showing(want);
    bsp_display_unlock();

    if (!ready) {
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
            ESP_LOGE(TAG, "TOUR %u/%u: %s moved DURING the capture -- slot "
                          "erased", slot + 1, (unsigned)PANEL_SHOT_SLOTS, what);
            panel_shot_erase_slot(slot);
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
    /* Long enough for the link to settle: a STATUS frame taken before that is
     * a picture of WAKING, which is a real state but not the resting one. It
     * is also past the wake frame that fires ~5 s in when no droid answers.
     * The whole tour finishes well inside the 60 s burn-in drift step, so the
     * nine frames share one alignment. */
    vTaskDelay(pdMS_TO_TICKS(12000));

    /* Blank every slot first, so a tour that dies half way leaves EMPTY slots
     * rather than the tail of the last one, which would decode perfectly. */
    if (!panel_shot_erase_all()) s_tour_ok = false;

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
             * is 2.98 MB of a 3 MB partition. The ladder loses, because it is
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
                /* ON ROWS_OK, NOT ON s_tour_ok. The tour-wide flag carries
                 * every earlier step's failure, and suppressing THIS picture
                 * because the VOICE interior had a bad moment would lose the
                 * evidence for the thing being changed. */
                if (rows_ok)
                    tour_shot(2 + i, "HW TEST ops after RUN", PANEL_TOUR_OPS);
            }
        } else {
            /* NO PICTURE AT ALL. The slot stays erased, so grab_tour.sh
             * reports NO FRAME: a missing picture is a finding, while one
             * filed under the wrong name is a lie the tool exists to avoid. */
            ESP_LOGE(TAG, "TOUR: tapping row %d did NOT open %s -- slot %u "
                          "left EMPTY", k_tour_row[i], k_tour_name[i], 2 + i);
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

static void on_sync(void) { r2_link_start(); }
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
