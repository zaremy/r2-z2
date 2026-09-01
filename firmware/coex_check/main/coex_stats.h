/* A1 coexistence measurement — the numbers the experiment exists to produce.
 *
 * Issue #103 A1 asks whether BLE and Wi-Fi can share this radio under OUR
 * traffic. The verdict is stated in advance:
 *   PASS      zero disconnects in all three arms, and no gap between successful
 *             keepalives exceeding 10 s (three missed) in any arm
 *   MARGINAL  arms 1-2 clean; arm 3 shows gaps >10 s but no disconnect
 *   FAIL      any disconnect, or any gap >10 s in arms 1-2
 *
 * So the only figures that matter are: disconnect count, and the longest gap
 * between two SUCCESSFUL keepalives. Everything here serves those two.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

void coex_stats_init(const char *arm_name);
void coex_note_connected(void);
void coex_note_disconnected(int reason);
void coex_note_keepalive_sent(uint8_t seq);
void coex_note_keepalive_ack(uint8_t seq, int status);
void coex_stats_report(void);
