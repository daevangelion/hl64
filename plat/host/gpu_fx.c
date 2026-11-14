/* SPDX-License-Identifier: MIT */
/*
 * gpu_fx.c -- the host's effects draw-list recorder (D5;
 * engine/render/gpu_fx.h): every effects call is counted per mode and
 * hashed (mode, RDP state, texture size, vertices, points), so a run is
 * comparable with itself; the last triangle batch is kept for the tests.
 * The frame's vertex ring is accounted as the cart fills it (pairs per
 * batch and frame matrix, GPU_FX_RING_PAIRS): what would not fit is
 * dropped and counted here too.
 */
#include <string.h>

#include "engine/render/gpu_fx.h"

static gpu_fx_stats_t	cur, done;
static int		mode = -1;
static gpu_fx_vert_t	last_v[64];
static int		last_n, last_mode = -1;
static ren_mat_t	frame, last_frame;
static int		framed, last_framed;
static int		used, full;	/* ring pairs this frame; a frame matrix did not fit */

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
	framed = 0;
	used = full = 0;
}

void gpu_fx_end(void)
{
	cur.pairs = (uint32_t)used;
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

void gpu_fx_ramp(const uint8_t prim[4], const uint8_t env[4])
{
	mix(prim, 4);
	mix(env, 4);
}

/* as on the cart: a draw needs a gpu_fx_mode after this */
void gpu_fx_state_lost(void)
{
	mode = -1;
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

void gpu_fx_frame(const ren_mat_t *m)
{
	static const ren_mat_t cam;	/* NULL: camera-relative, hashed as zeros */

	mix(m ? m : &cam, sizeof(cam));
	framed = m != NULL;
	full = 0;
	if (m)
	{
		frame = *m;
		if (used + GPU_FX_MAT_PAIRS > GPU_FX_RING_PAIRS)
			full = 1;
		else
			used += GPU_FX_MAT_PAIRS;
	}
}

void gpu_fx_tris(const gpu_fx_vert_t *v, int nv, const uint8_t *idx, int ntri)
{
	int pairs = (nv + 1) / 2;

	if (mode < 0 || nv <= 0 || ntri <= 0 || nv > 64)
		return;
	if (full || used + pairs > GPU_FX_RING_PAIRS)
	{
		cur.calls[mode]++;	/* as the cart: counted, not drawn */
		cur.drops++;
		return;
	}
	used += pairs;
	mix(v, (uint32_t)nv * sizeof(*v));
	mix(idx, (uint32_t)ntri * 3);
	cur.calls[mode]++;
	cur.tris += (uint32_t)ntri;
	cur.verts += (uint32_t)nv;
	last_n = nv;
	memcpy(last_v, v, sizeof(*v) * (size_t)last_n);
	last_mode = mode;
	last_frame = frame;
	last_framed = framed;
}

int gpu_fx_host_last(gpu_fx_vert_t *out, int max, int *m)
{
	int n = last_n < max ? last_n : max;

	memcpy(out, last_v, sizeof(*out) * (size_t)n);
	*m = last_mode;
	return n;
}

int gpu_fx_host_last_frame(ren_mat_t *m)
{
	*m = last_frame;
	return last_framed;
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
