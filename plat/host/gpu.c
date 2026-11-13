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

#include "engine/render/gpu.h"

static gpu_stats_t	cur, done;

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
	(void)state;
	mix(&p->w, 4);
	mix((const uint8_t *)p + sizeof(*p), 32);	/* TLUT */
	cur.binds++;
}

void gpu_draw(const w64_vert_t *v, int nv, const int16_t *idx, int ni)
{
	int k, faces = 0;

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
