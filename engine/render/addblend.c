/* SPDX-License-Identifier: MIT */
/*
 * addblend.c -- additive draws without the RDP adder's wrap
 * (docs/specs/render-fx.md 2a; the interface note is addblend.h).
 *
 * Why: the RDP blender sums in x a + mem x b in 8 bits per channel and
 * keeps the low 8 bits (no clamp). An additive item over a surface already
 * brighter than 255 - c turns black or a false colour (measured on ares,
 * whose parallel-RDP blender is the angrylion integer model: (P a0 +
 * M (b0 + 1)) >> 5, truncated to u8). Capping the blend cannot help: the
 * framebuffer can already be at 255.
 *
 * What: out = n x a + mem x (1 - a) through the alpha blender, n a unit
 * colour, a its brightness; (n a0 + M (32 - a0)) >> 5 <= 255 for any a0,
 * n, M, so no wrap is possible. The source term n x a is GoldSrc's c.
 * Textures bind the ramp the cook fitted (tools/cook/addramp.py): fitting
 * on the cart cost up to 4 ms per sprite load on ares.
 */
#include <string.h>

#include "engine/render/addblend.h"

static add_stats_t	st;

void add_ramp_ends(const add_ramp_t *r, const uint8_t tint[3], uint8_t blend, uint8_t prim[4],
	uint8_t env[4])
{
	int k;

	for (k = 0; k < 3; k++)
	{
		prim[k] = (uint8_t)((r->hi[k] * tint[k] + 127) / 255);
		env[k] = (uint8_t)((r->lo[k] * tint[k] + 127) / 255);
	}
	prim[3] = blend;
	env[3] = 255;
}

void add_split(const uint8_t rgb[3], uint8_t a, uint8_t out[4])
{
	int m = rgb[0] > rgb[1] ? rgb[0] : rgb[1], k;

	m = m > rgb[2] ? m : rgb[2];
	if (!m)
	{
		memset(out, 0, 4);
		return;
	}
	for (k = 0; k < 3; k++)
		out[k] = (uint8_t)((rgb[k] * 255 + m / 2) / m);
	out[3] = (uint8_t)((m * a + 127) / 255);
}

const add_ramp_t *add_page_ramp(const w64_texp_t *p)
{
	if (!(p->flags & W64_TEXF_RAMP))
	{
		st.miss++;
		return NULL;
	}
	st.pages++;
	return (const add_ramp_t *)((const uint8_t *)p + W64_TEXP_RAMP);
}

void add_stats(add_stats_t *s)
{
	*s = st;
}
