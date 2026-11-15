/* SPDX-License-Identifier: MIT */
/*
 * gpu_2d.c -- the cart's 2D blits (engine/render/gpu_2d.h) through rdpq:
 * each sheet is an I4 surface over the HUD region; a sub-rectangle is
 * drawn with the primitive colour as its colour and texel x primitive
 * alpha as its coverage, alpha-blended; D2_ADD (GoldSrc's
 * SPR_DrawAdditive) arrives as unit colour x brightness from draw2d.c
 * (render-fx.md 2a: the RDP adder wraps). The mode is set once for a run of
 * blits with the same blend and colour. From the old tree's r64_draw.c
 * HUD sheet blits (provenance-audit.md: lines 246-375 CLEAN), retargeted
 * from CI8 + TLUT to I4.
 */
#include <string.h>

#include <libdragon.h>
#include <rdpq_tex.h>

#include "hl64/draw2d.h"
#include "engine/render/gpu_2d.h"

/* game/defs/hud.def's sheets (7) + engine/render/draw2d.c D2_DYN (2) */
#define SHEETS	9

static surface_t	sheet[SHEETS];
static int		cur_mode = -1;
static uint32_t		cur_prim;
static gpu_2d_stats_t	cur, done;

void gpu_2d_sheet(int id, const uint8_t *i4, int w, int h, int stride)
{
	if (id < 0 || id >= SHEETS)
		return;
	data_cache_hit_writeback((void *)i4, (unsigned)(stride * h));
	sheet[id] = surface_make((void *)i4, FMT_I4, (uint16_t)w, (uint16_t)h, (uint16_t)stride);
}

void gpu_2d_begin(void)
{
	memset(&cur, 0, sizeof(cur));
	cur_mode = -1;
}

void gpu_2d_end(void)
{
	done = cur;
	cur_mode = -1;
}

static void set_mode(int mode, int tex, const uint8_t rgba[4])
{
	int m = mode * 2 + tex;
	uint32_t prim = (uint32_t)rgba[0] << 24 | (uint32_t)rgba[1] << 16 | (uint32_t)rgba[2] << 8
		| rgba[3];

	if (m != cur_mode)
	{
		rdpq_set_mode_standard();
		if (tex)
			rdpq_mode_combiner(RDPQ_COMBINER1((0, 0, 0, PRIM), (TEX0, 0, PRIM, 0)));
		else
			rdpq_mode_combiner(RDPQ_COMBINER_FLAT);
		rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
		cur_mode = m;
		cur_prim = ~prim;
	}
	if (prim != cur_prim)
	{
		rdpq_set_prim_color(RGBA32(rgba[0], rgba[1], rgba[2], rgba[3]));
		cur_prim = prim;
	}
}

void gpu_2d_pic(int id, int sx, int sy, int w, int h, int x, int y, const uint8_t rgba[4],
	int mode)
{
	if (id < 0 || id >= SHEETS || !sheet[id].buffer)
		return;
	set_mode(mode, 1, rgba);
	rdpq_tex_blit(&sheet[id], (float)x, (float)y, &(rdpq_blitparms_t){
		.s0 = sx, .t0 = sy, .width = w, .height = h });
	cur.pics++;
	cur.texels += (uint32_t)(w * h);
}

void gpu_2d_fill(int x, int y, int w, int h, const uint8_t rgba[4], int mode)
{
	set_mode(mode, 0, rgba);
	rdpq_fill_rectangle(x, y, x + w, y + h);
	cur.fills++;
}

void gpu_2d_stats(gpu_2d_stats_t *s)
{
	*s = done;
}
