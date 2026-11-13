/* SPDX-License-Identifier: MIT */
/*
 * draw2d.h -- 2D services for game/hud (docs/architecture.md 7.1 "HUD/2D",
 * 9.8): ui8 sheet sub-rectangles with colour and blend mode, fills and
 * glyphs, in 320x240 screen pixels. Sheets are cooked ui8 files
 * (gfx/<name>.ui8) addressed by the ids of game/defs/hud.def; nothing is
 * looked up by name at run time. The host recorder logs every call.
 */
#ifndef HL64_DRAW2D_H
#define HL64_DRAW2D_H

#include <stdint.h>

#define D2_NORMAL		0	/* alpha-tested */
#define D2_ADD			1	/* additive (HLSDK SPR_DrawAdditive) */
#define D2_HOLES		2	/* SPR_DrawHoles */
#define D2_ALPHA		3	/* FillRGBA blend */

int	d2_sheet_w(int sheet);
int	d2_sheet_h(int sheet);
/* Sub-rect (sx, sy, w, h) of sheet at (x, y), tinted by r, g, b; a
 * scales the tint (HUD alpha). */
void	d2_pic(int sheet, int sx, int sy, int w, int h, int x, int y,
		uint8_t r, uint8_t g, uint8_t b, uint8_t a, int mode);
void	d2_fill(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a,
		int mode);
/* Text in the player's HL font (cooked), returns the width drawn. */
int	d2_text(int x, int y, const char *s, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
void	d2_screen(int *w, int *h);
/* Row height of the text font, 0 when the font is missing. */
int	d2_text_height(void);

/* Engine: boot (sheets resident in the HUD region), frame begin / flush
 * after the 3D draw. */
void	d2_init(void);
void	d2_begin(void);
void	d2_end(void);
/* PROFILE roll-up: CPU us per frame (avg, max), draws per frame, HUD
 * region bytes; resets the window. */
void	d2_rollup(uint32_t *avg, uint32_t *max, uint32_t *draws, uint32_t *bytes);

#endif
