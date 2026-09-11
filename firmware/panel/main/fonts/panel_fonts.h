/* THE SPEC'S OWN TYPEFACES, converted for LVGL.
 *
 * The v5 reference is set in Michroma (a wide, squarish display face) and
 * Share Tech Mono (a monospace, for values). Neither ships with LVGL, so the
 * panel drew everything in Montserrat with letter-spacing standing in for
 * Michroma's width -- a deviation three separate comments in `panel_ui.c`
 * had to apologise for. This retires it.
 *
 * WHY A MONOSPACE FOR THE VALUES IS NOT A STYLE CHOICE: the readings change
 * while you are looking at them. In a proportional face 4.43 -> 4.42 shifts
 * every digit left or right by a fraction, so the number appears to twitch;
 * in a monospace each digit occupies the same cell and only the glyph that
 * changed changes. The reference chose it for the same reason instrument
 * panels always have.
 *
 * Generated with lv_font_conv 1.5.3, 4 bpp, range 0x20-0x5F plus 0xB0:
 * uppercase, digits, punctuation and the degree sign. techmono_72 is the
 * one exception: space, hyphen, digits and uppercase only, because it sets
 * nothing but the wake frame's boxed subject and each glyph at 72 px is
 * large enough that unused punctuation is not free. techmono_26 adds the
 * two chevrons (U+2039, U+203A) the SERVICE rows and back button use, and
 * techmono_14 the middle dot (U+00B7) in the ladder's footer. NO LOWERCASE -- every
 * string on the status face is uppercase, and dropping it roughly halves the
 * flash. Anything that needs lowercase (the NETWORK page's "no Wi-Fi in this
 * build") stays on Montserrat, which is still built.
 *
 * Sizes are the REFERENCE's, measured off the prototype, not Montserrat's:
 * the panel had been carrying sizes chosen to make a substitute face fit.
 *
 * Both faces are SIL Open Font License 1.1; the licences sit beside this
 * header and travel with the source, which is what the OFL requires.
 */
#ifndef PANEL_FONTS_H
#define PANEL_FONTS_H

#include "lvgl.h"

/* Michroma -- the display face. Words and labels. */
LV_FONT_DECLARE(michroma_12)    /* chrome: LLM, PWR */
LV_FONT_DECLARE(michroma_13)    /* chain node labels */
LV_FONT_DECLARE(michroma_16)    /* R2 PWR, DOME */
LV_FONT_DECLARE(michroma_30)    /* the state word */
LV_FONT_DECLARE(michroma_32)    /* the wake frame's word */

/* Share Tech Mono -- values, and the reason line under the word. */
LV_FONT_DECLARE(techmono_14)    /* the ladder's footer */
LV_FONT_DECLARE(techmono_18)    /* SERVICE notes, a rung's lock */
LV_FONT_DECLARE(techmono_20)    /* a reading's unit */
LV_FONT_DECLARE(techmono_22)    /* the wake frame's box lines */
LV_FONT_DECLARE(techmono_24)    /* the reason line, a ladder rung */
LV_FONT_DECLARE(techmono_26)    /* SERVICE rows and interior headers */
LV_FONT_DECLARE(techmono_28)    /* the wake frame's reason, a SERVICE value */
LV_FONT_DECLARE(techmono_34)    /* a reading's number */
LV_FONT_DECLARE(techmono_72)    /* the wake frame's boxed subject */

#endif /* PANEL_FONTS_H */
