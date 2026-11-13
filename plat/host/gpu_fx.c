/* SPDX-License-Identifier: MIT */
/*
 * gpu_fx.c -- the host's effects draw-list recorder (D5;
 * engine/render/gpu_fx.h): every effects call is counted per mode and
 * hashed (mode, RDP state, texture size, vertices, points), so a run is
 * comparable with itself and the X1 test reads the state each mode set.
 */
#include <string.h>

#include "engine/render/gpu_fx.h"

static gpu_fx_stats_t	cur, done;
static int		mode = -1;

static void mix(const void *p, uint32_t n)
{
	const uint8_t *b = p;

	while (n--)
		cur.hash = (cur.hash ^ *b++) * 16777619u;
}

void gpu_fx_begin(void)
{
	memset(&cur, 0, sizeof(cur));
	cur.hash = 2166136261u;
	mode = -1;
}

void gpu_fx_end(void)
{
	done = cur;
}

void gpu_fx_mode(int m, const uint8_t prim[4])
{
	if (m < 0 || m >= GPU_FX_MODES)
		return;
	mode = m;
	mix(&m, sizeof(m));
	mix(&gpu_fx_rdp[m], sizeof(gpu_fx_rdp[m]));
	if (prim)
		mix(prim, 4);
}

void gpu_fx_tex_ci4(const uint16_t *tlut, const uint8_t *ci4, int w, int h)
{
	(void)ci4;
	mix(&w, sizeof(w));
	mix(&h, sizeof(h));
	if (tlut)
		mix(tlut, 32);
	cur.texloads++;
}

void gpu_fx_tex_i4(const uint8_t *i4, int w, int h)
{
	mix(&w, sizeof(w));
	mix(&h, sizeof(h));
	if (i4)
		mix(i4, 8);
	cur.texloads++;
}

void gpu_fx_tris(const gpu_fx_vert_t *v, int nv, const uint8_t *idx, int ntri)
{
	if (mode < 0 || nv <= 0 || ntri <= 0)
		return;
	mix(v, (uint32_t)nv * sizeof(*v));
	mix(idx, (uint32_t)ntri * 3);
	cur.calls[mode]++;
	cur.tris += (uint32_t)ntri;
	cur.verts += (uint32_t)nv;
}

void gpu_fx_points(const gpu_fx_point_t *p, int n)
{
	if (mode < 0 || n <= 0)
		return;
	mix(p, (uint32_t)n * sizeof(*p));
	cur.calls[mode]++;
	cur.points += (uint32_t)n;
}

void gpu_fx_stats(gpu_fx_stats_t *s)
{
	*s = done;
}
