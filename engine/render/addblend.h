/* SPDX-License-Identifier: MIT */
/*
 * addblend.h -- additive draws without the RDP adder's wrap
 * (docs/specs/render-fx.md 2a).
 *
 * Every additive item (sprites, glows, beams, tracer streaks, brush and
 * studio TransAdd / Glow, the HUD's additive pictures and text) goes
 * through the ordinary alpha blender, out = in x a + mem x (1 - a), with
 * its source colour c (GoldSrc's additive term) split as c = n x a: a unit
 * colour n (max channel 255) times its brightness a. The result
 * c + mem x (1 - a) stays <= 255 in every channel whatever the framebuffer
 * holds and however many items stack; the background dims by (1 - a)
 * instead of saturating.
 *
 * Textures are CI4 with a 16-entry RGBA5551 TLUT; the RDP can only take a
 * per-texel alpha from the texel, so an additive texture binds an IA16
 * ramp TLUT: A = the entry's brightness, I = its place between two unit
 * colours lo (ENV) and hi (PRIM) that the combiner interpolates
 * (rgb = (PRIM - ENV) x I + ENV, a = A x PRIM alpha). The cook fits the
 * ramp (tools/cook/addramp.py) and stores it in the sprite file, the
 * additive brush pages and the additive studio skins
 * (docs/formats/sc4-format.md, w64-format.md TEXP, studio-format.md); the
 * cart only binds it.
 */
#ifndef HL64_ADDBLEND_H
#define HL64_ADDBLEND_H

#include <stdint.h>

#include "engine/world/w64.h"

typedef struct {
	uint16_t	ia[16];		/* IA16 TLUT (I high byte): ramp place, brightness */
	uint8_t		lo[3], hi[3];	/* ramp ends: unit colours (ENV, PRIM) */
	uint8_t		pad[2];
} add_ramp_t;			/* 40, cooked layout; the GPU reads ia: 8-aligned */

_Static_assert(sizeof(add_ramp_t) == 40, "add_ramp_t");

/* PRIM (hi x tint, alpha = blend) and ENV (lo x tint) for a ramp draw. */
void	add_ramp_ends(const add_ramp_t *r, const uint8_t tint[3], uint8_t blend, uint8_t prim[4],
		uint8_t env[4]);
/* An untextured or I4 additive colour (rgb x a added): the unit colour in
 * out[0..2] and the alpha in out[3] that the alpha blender adds as the
 * same term. */
void	add_split(const uint8_t rgb[3], uint8_t a, uint8_t out[4]);

/* The cooked ramp of a page bound for an additive draw (TEXP or studio
 * skin, W64_TEXF_RAMP); NULL, counted, when the cook gave it none (the
 * page then adds nothing). */
const add_ramp_t *add_page_ramp(const w64_texp_t *p);

typedef struct {
	uint32_t	pages;		/* additive page binds with a cooked ramp, since boot */
	uint32_t	miss;		/* additive page binds without one (not drawn) */
} add_stats_t;
void	add_stats(add_stats_t *s);

#endif
