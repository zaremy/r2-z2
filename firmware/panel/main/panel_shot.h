#ifndef PANEL_SHOT_H
#define PANEL_SHOT_H

#include <stdbool.h>

/* Capture the active LVGL screen into the 'storage' partition, where
 * tools/grab_shot.sh reads it back over the same cable. Takes the display
 * lock itself; do NOT hold it. See panel_shot.c for the format.
 *
 * The partition holds several frames side by side, so one boot can record a
 * whole tour of the UI and a single esptool read fetches all of it -- each
 * read RESETS the board, so a frame per read would be a boot per frame. */
#define PANEL_SHOT_SLOT_BYTES 0x52000u   /* 4 KB-aligned, >= a 368x448 frame */
#define PANEL_SHOT_SLOTS      9u         /* 9 x 0x52000 fits the 3 MB partition */

/* Both return false if the frame did not reach flash. A caller that files
 * the picture under a name -- the tour -- must be able to tell an empty slot
 * from a written one, because an empty slot is the honest outcome and a
 * silent one is how a missing frame becomes an unnoticed gap. */
bool panel_shot_take(void);                   /* slot 0 */
bool panel_shot_take_slot(unsigned slot);

/* Erase every slot. The tour calls this before it starts: a run that dies
 * half way would otherwise leave the REST of a previous tour in place, and
 * those frames decode perfectly -- a stale picture that looks like a fresh
 * one is the failure this whole tool exists to avoid. False if the erase
 * failed, which means exactly that danger is still present. */
bool panel_shot_erase_all(void);

/* Erase ONE slot: for a caller that captured a frame and then found it was
 * not of what it meant to photograph. Blanking it turns a wrong picture into
 * a missing one, which is the honest outcome. */
bool panel_shot_erase_slot(unsigned slot);
#endif
