/* SPDX-License-Identifier: MIT */
/*
 * gpu_2d.c -- the host's 2D draw-list recorder (D5; engine/render/gpu_2d.h):
 * counts sub-rectangle blits, fills and texels and hashes every call
 * (sheet, rectangles, colour, mode) so a route's HUD is comparable with
 * itself across changes.
 */
#include <string.h>

#include "engine/render/gpu_2d.h"

static gpu_2d_stats_t	cur, done;

static void mix(const void *p, uint32_t n)
{
	const uint8_t *b = p;

	while (n--)
		cur.hash = (cur.hash ^ *b++) * 16777619u;
}

void gpu_2d_sheet(int id, const uint8_t *i4, int w, int h, int stride)
{
	(void)id; (void)i4; (void)w; (void)h; (void)stride;
}

void gpu_2d_begin(void)
{
	memset(&cur, 0, sizeof(cur));
	cur.hash = 2166136261u;
}

void gpu_2d_end(void)
{
	done = cur;
}

void gpu_2d_pic(int id, int sx, int sy, int w, int h, int x, int y, const uint8_t rgba[4],
	int mode)
{
	int16_t v[8] = { (int16_t)id, (int16_t)sx, (int16_t)sy, (int16_t)w, (int16_t)h,
		(int16_t)x, (int16_t)y, (int16_t)mode };

	cur.pics++;
	cur.texels += (uint32_t)(w * h);
	mix(v, sizeof(v));
	mix(rgba, 4);
}

void gpu_2d_fill(int x, int y, int w, int h, const uint8_t rgba[4], int mode)
{
	int16_t v[5] = { (int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h, (int16_t)mode };

	cur.fills++;
	mix(v, sizeof(v));
	mix(rgba, 4);
}

void gpu_2d_stats(gpu_2d_stats_t *s)
{
	*s = done;
}
