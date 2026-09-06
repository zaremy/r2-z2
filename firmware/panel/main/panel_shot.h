#ifndef PANEL_SHOT_H
#define PANEL_SHOT_H
/* Dump the active LVGL screen over serial as RLE+base64. Must hold the LVGL
 * lock. See panel_shot.c for the wire format. */
void panel_shot_take(void);
#endif
