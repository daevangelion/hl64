/* SPDX-License-Identifier: MIT */
/*
 * gpu.c -- the host's draw-list recorder (D5; engine/render/gpu.h): the
 * renderer's GPU calls are counted instead of drawn. Per frame: binds,
 * meshlets, vertices, triangles (from the strip's restart bits) and an
 * FNV-1a hash of what was bound and drawn (page size and palette, strip
 * contents, first vertex position), so a run is comparable with the cook's
 * prediction (test/m1/predict.py) and with itself across changes.
 * The cooked strip encoding is read as is (no tiny3d on the host).
 */
#include <string.h>

#include "engine/core/sys.h"
#include "engine/render/addblend.h"
#include "engine/render/gpu.h"

static gpu_stats_t	cur, done;
static uint8_t		ramp_tb[4];	/* gpu_page_ramp tint + blend */
static int		ramp_on;

static void mix(const void *p, uint32_t n)
{
	const uint8_t *b = p;

	while (n--)
		cur.hash = (cur.hash ^ *b++) * 16777619u;
}

int gpu_index_check(unsigned stride, unsigned base)
{
	(void)stride;
	(void)base;
	return 0;
}

void gpu_index_reencode(int16_t *idx, int n, unsigned stride, unsigned base)
{
	(void)idx;
	(void)n;
	(void)stride;
	(void)base;
}

void gpu_begin(const gpu_view_t *v)
{
	(void)v;
	memset(&cur, 0, sizeof(cur));
	cur.hash = 2166136261u;
}

void gpu_end(void)
{
	done = cur;
}

void gpu_matrix_push(const ren_mat_t *m)
{
	mix(m->m[3], sizeof(m->m[3]));
}

void gpu_matrix_pop(void)
{
}

void gpu_bind_page(const w64_texp_t *p, uint8_t *state)
{
	const uint16_t *tlut = (const uint16_t *)((const uint8_t *)p + sizeof(*p));

	(void)state;
	mix(&p->w, 4);
	mix(tlut, 32);
	if (ramp_on)
	{
		/* as the cart: the page's cooked ramp TLUT and ends (render-fx.md 2a) */
		const add_ramp_t *r = add_page_ramp(p);
		uint8_t prim[4] = { 0, 0, 0, 0 }, env[4] = { 0, 0, 0, 0 };

		if (r)
		{
			add_ramp_ends(r, ramp_tb, ramp_tb[3], prim, env);
			mix(r->ia, sizeof(r->ia));
		}
		mix(prim, 4);
		mix(env, 4);
	}
	cur.binds++;
}

void gpu_page_ramp(const uint8_t tint_blend[4])
{
	ramp_on = tint_blend != NULL;
	if (ramp_on)
		memcpy(ramp_tb, tint_blend, 4);
}

/* the recorder keeps no RDP state */
void gpu_state_lost(void)
{
}

void gpu_decal_depth(int on)
{
	mix(&on, sizeof(on));
}

void gpu_draw(const w64_vert_t *v, int nv, const int16_t *idx, int ni)
{
	int k, faces = 0;

	/* the cart's RSP DMA alignment (gpu.h) */
	if (((uintptr_t)v | (uintptr_t)idx) & 7)
		sys_error("gpu: draw with vertices %p / strip %p not 8-byte aligned",
			(const void *)v, (const void *)idx);
	for (k = 0; k < ni; k++)
		faces += (idx[k] & W64_IDX_RESTART) != 0;
	mix(v->posA, sizeof(v->posA));
	mix(idx, (uint32_t)ni * 2);
	cur.meshlets++;
	cur.verts += (uint32_t)nv;
	cur.tris += (uint32_t)(ni - 2 * faces);
}

void gpu_writeback(const void *p, uint32_t n)
{
	(void)p;
	(void)n;
}

void gpu_stats(gpu_stats_t *s)
{
	*s = done;
}
