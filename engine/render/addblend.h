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
 * per-texel alpha from the texel, so the TLUT becomes an IA16 ramp TLUT:
 * A = the entry's brightness, I = its place between two unit colours lo
 * (ENV) and hi (PRIM) that the combiner interpolates
 * (rgb = (PRIM - ENV) x I + ENV, a = A x PRIM alpha).
 */
#ifndef HL64_ADDBLEND_H
#define HL64_ADDBLEND_H

#include <stdint.h>

typedef struct {
	uint16_t	ia[16];		/* IA16 TLUT (I high byte): ramp place, brightness */
	uint8_t		lo[3], hi[3];	/* ramp ends: unit colours (ENV, PRIM) */
	uint8_t		pad[2];
} add_ramp_t;			/* 40; the GPU reads ia: keep it 8-aligned */

/* The ramp of a 16-entry RGBA5551 TLUT (native order): of the palette's
 * own unit colours, the pair (lo, hi) whose non-negative combinations
 * fit the lit entries best (squared error weighted by brightness, every
 * pair tried); entries with the alpha bit clear or black get A = 0. */
void	add_ramp_fit(add_ramp_t *r, const uint16_t tlut[16]);
/* PRIM (hi x tint, alpha = blend) and ENV (lo x tint) for a ramp draw. */
void	add_ramp_ends(const add_ramp_t *r, const uint8_t tint[3], uint8_t blend, uint8_t prim[4],
		uint8_t env[4]);
/* An untextured or I4 additive colour (rgb x a added): the unit colour in
 * out[0..2] and the alpha in out[3] that the alpha blender adds as the
 * same term. */
void	add_split(const uint8_t rgb[3], uint8_t a, uint8_t out[4]);

/* add_ramp_fit, timed and counted, the ramp's ia written back for the
 * GPU (sprites fit theirs when their file lands). */
void	add_ramp_make(add_ramp_t *r, const uint16_t tlut[16]);
/* Page ramps (brush and studio TransAdd draw world / skin pages): fitted
 * on first use and cached by TLUT content (HL64_FX_ADD_PAGES entries); an
 * entry the GPU may still read (used in this or the previous two frames)
 * is never replaced: NULL when none is free (the page is not drawn,
 * counted). The returned ramp's ia is written back for the GPU. */
const add_ramp_t *add_ramp_page(const uint16_t tlut[16]);
/* Frame boundary (the effects pass's start). */
void	add_frame(void);

typedef struct {
	uint32_t	fits;		/* ramps fitted (sprites and pages), since boot */
	uint32_t	fit_us_sum, fit_us_max;
	uint32_t	hits, full;	/* page ramps found in the cache / refused */
} add_stats_t;
void	add_stats(add_stats_t *s);

#endif
