/* SPDX-License-Identifier: MIT */
/*
 * gpu_fx.c -- the effects renderer's GPU seam on the cart
 * (engine/render/gpu_fx.h): tiny3d triangles for sprites, beams, streaks
 * and decals, TinyPX for particles, rdpq render modes from the shared
 * gpu_fx_rdp table.
 *
 * The TinyPX particle submit (identity-relative s16 buffer, mode push, PRIM
 * colour, Z through the particle ucode, base size and scale) follows the
 * old tree's n64/port/r64_part.c D_EndParticles (audit entry r64_part.c:
 * QUAKE-INTERFACE, CLEAN content; its D_* hooks and Quake types are gone).
 *
 * Vertices go to a two-frame uncached ring (the RSP reads a frame's
 * vertices while the CPU fills the next); positions are camera-relative
 * quarter units under one scale matrix per batch (pushed and popped around
 * each batch, so the world's mover matrices are untouched), or model-frame
 * units under a gpu_fx_frame matrix kept in the same ring.
 */
#include <string.h>

#include <libdragon.h>
#include <rdpq_tex.h>
#include <t3d/t3d.h>
#include <t3d/tpx.h>

#include "hl64/limits.h"
#include "engine/core/sys.h"
#include "engine/render/gpu_fx.h"

#define RING_PAIRS	1024		/* vertex pairs per frame */
#define MAT_PAIRS	(int)(sizeof(T3DMat4FP) / sizeof(T3DVertPacked))
#define POINTS_MAX	HL64_FX_PARTICLES

_Static_assert(sizeof(T3DMat4FP) % sizeof(T3DVertPacked) == 0, "frame matrix in ring pairs");

static T3DVertPacked	*ring;		/* 2 x RING_PAIRS, uncached */
static TPXParticleS16	*pts;		/* 2 x POINTS_MAX / 2, uncached */
static T3DMat4FP	*mat;		/* the quarter-unit scale */
static T3DMat4FP	*frame;		/* batch matrix: mat or a ring entry; NULL = ring full */
static int		ready, half, used, mode = -1;
static gpu_fx_stats_t	cur, done;

static void boot(void)
{
	float s = 1.0f / GPU_FX_POS_SCALE;
	float sc[3] = { s, s, s }, rot[3] = { 0, 0, 0 }, pos[3] = { 0, 0, 0 };

	ring = malloc_uncached(2 * RING_PAIRS * sizeof(T3DVertPacked));
	pts = malloc_uncached(2 * (POINTS_MAX / 2 + 1) * sizeof(TPXParticleS16));
	mat = malloc_uncached(sizeof(T3DMat4FP));
	if (!ring || !pts || !mat)
		sys_error("gpu_fx: no uncached memory (%u B)",
			(unsigned)(2 * RING_PAIRS * sizeof(T3DVertPacked)
			+ 2 * (POINTS_MAX / 2 + 1) * sizeof(TPXParticleS16)));
	t3d_mat4fp_from_srt_euler(mat, sc, rot, pos);
	tpx_init((TPXInitParams){});
	ready = 1;
}

void gpu_fx_begin(void)
{
	if (!ready)
		boot();
	half ^= 1;
	used = 0;
	mode = -1;
	frame = mat;
	memset(&cur, 0, sizeof(cur));
}

void gpu_fx_end(void)
{
	/* the world's state for whoever draws next (view model, HUD) */
	rdpq_set_mode_standard();
	rdpq_mode_combiner(RDPQ_COMBINER_TEX_SHADE);
	rdpq_mode_tlut(TLUT_RGBA16);
	rdpq_mode_filter(FILTER_BILINEAR);
	rdpq_mode_persp(true);
	rdpq_mode_zbuf(true, true);
	t3d_state_set_drawflags(T3D_FLAG_SHADED | T3D_FLAG_TEXTURED | T3D_FLAG_DEPTH
		| T3D_FLAG_NO_LIGHT | T3D_FLAG_CULL_FRONT);
	gpu_state_lost();	/* alpha compare now off, whatever the world's cache says */
	done = cur;
}

static rdpq_combiner_t combiner(int cc)
{
	switch (cc)
	{
	case GPU_FX_CC_SHADE:
		return RDPQ_COMBINER1((0, 0, 0, SHADE), (0, 0, 0, SHADE));
	case GPU_FX_CC_SHADE_TEXA:
		return RDPQ_COMBINER1((0, 0, 0, SHADE), (TEX0, 0, SHADE, 0));
	case GPU_FX_CC_PRIM:
		return RDPQ_COMBINER1((0, 0, 0, PRIM), (0, 0, 0, 1));
	case GPU_FX_CC_TEX_PRIM:
		return RDPQ_COMBINER1((TEX0, 0, PRIM, 0), (0, 0, 0, PRIM));
	case GPU_FX_CC_PRIM_A:
		return RDPQ_COMBINER1((0, 0, 0, PRIM), (0, 0, 0, PRIM));
	default:
		return RDPQ_COMBINER1((TEX0, 0, SHADE, 0), (TEX0, 0, SHADE, 0));
	}
}

void gpu_fx_mode(int m, const uint8_t prim[4])
{
	const gpu_fx_rdp_t *r;
	uint32_t flags = T3D_FLAG_SHADED | T3D_FLAG_DEPTH | T3D_FLAG_NO_LIGHT;

	if (m < 0 || m >= GPU_FX_MODES)
		return;
	r = &gpu_fx_rdp[m];
	if (prim)
		rdpq_set_prim_color(RGBA32(prim[0], prim[1], prim[2], prim[3]));
	if (m == mode)
		return;
	mode = m;
	rdpq_set_mode_standard();
	rdpq_mode_combiner(combiner(r->combiner));
	rdpq_mode_blender(r->blender == GPU_FX_BL_ADD ? RDPQ_BLENDER_ADDITIVE
		: r->blender == GPU_FX_BL_ALPHA ? RDPQ_BLENDER_MULTIPLY : 0);
	rdpq_mode_zbuf(r->ztest, r->zwrite);
	if (r->zdecal)
		rdpq_mode_zmode(ZMODE_DECAL);
	if (m == GPU_FX_POINT)
		rdpq_mode_zoverride(true, 0, 0);
	rdpq_mode_alphacompare(r->acompare);
	gpu_state_lost();	/* a translucent brush's page bind re-applies its fence */
	rdpq_mode_tlut(r->textured == 1 || r->textured == 3 ? TLUT_RGBA16 : TLUT_NONE);
	rdpq_mode_filter(FILTER_BILINEAR);
	rdpq_mode_persp(true);
	if (r->textured)
		flags |= T3D_FLAG_TEXTURED;
	t3d_state_set_drawflags(flags);
}

void gpu_fx_state_lost(void)
{
	mode = -1;
}

void gpu_fx_tex_ci4(const uint16_t *tlut, const uint8_t *ci4, int w, int h)
{
	surface_t s;

	if (!tlut || !ci4)
		return;
	s = surface_make((void *)ci4, FMT_CI4, w, h, w / 2);
	rdpq_tex_upload_tlut((uint16_t *)tlut, 0, 16);
	rdpq_tex_upload(TILE0, &s, &(rdpq_texparms_t){
		.s.repeats = REPEAT_INFINITE, .t.repeats = REPEAT_INFINITE });
	cur.texloads++;
}

void gpu_fx_tex_i4(const uint8_t *i4, int w, int h)
{
	surface_t s;

	if (!i4)
		return;
	s = surface_make((void *)i4, FMT_I4, w, h, w / 2);
	rdpq_tex_upload(TILE0, &s, NULL);
	cur.texloads++;
}

static void put(T3DVertPacked *p, int b, const gpu_fx_vert_t *v)
{
	uint32_t c = (uint32_t)v->rgba[0] << 24 | (uint32_t)v->rgba[1] << 16
		| (uint32_t)v->rgba[2] << 8 | v->rgba[3];

	if (!b)
	{
		memcpy(p->posA, v->pos, sizeof(p->posA));
		p->normA = 0;
		p->rgbaA = c;
		p->stA[0] = v->st[0];
		p->stA[1] = v->st[1];
	}
	else
	{
		memcpy(p->posB, v->pos, sizeof(p->posB));
		p->normB = 0;
		p->rgbaB = c;
		p->stB[0] = v->st[0];
		p->stB[1] = v->st[1];
	}
}

void gpu_fx_frame(const ren_mat_t *m)
{
	if (!m)
		frame = mat;
	else if (used + MAT_PAIRS > RING_PAIRS)
		frame = NULL;	/* the following batches are counted, not drawn */
	else
	{
		frame = (T3DMat4FP *)&ring[half * RING_PAIRS + used];
		used += MAT_PAIRS;
		t3d_mat4_to_fixed_3x4(frame, (const T3DMat4 *)m);
	}
}

void gpu_fx_tris(const gpu_fx_vert_t *v, int nv, const uint8_t *idx, int ntri)
{
	T3DVertPacked *p;
	int pairs = (nv + 1) / 2, i;

	if (mode < 0 || nv <= 0 || ntri <= 0 || nv > 64)
		return;
	if (!frame || used + pairs > RING_PAIRS)
	{
		cur.calls[mode]++;	/* counted, not drawn: the frame's ring is full */
		return;
	}
	p = &ring[half * RING_PAIRS + used];
	used += pairs;
	for (i = 0; i < nv; i++)
		put(&p[i >> 1], i & 1, &v[i]);
	t3d_matrix_push(frame);
	t3d_vert_load(p, 0, (uint32_t)nv);
	for (i = 0; i < ntri; i++)
		t3d_tri_draw(idx[3 * i], idx[3 * i + 1], idx[3 * i + 2]);
	t3d_tri_sync();
	t3d_matrix_pop(1);
	cur.calls[mode]++;
	cur.tris += (uint32_t)ntri;
	cur.verts += (uint32_t)nv;
}

void gpu_fx_points(const gpu_fx_point_t *p, int n)
{
	TPXParticleS16 *b = &pts[half * (POINTS_MAX / 2 + 1)];
	int i;

	if (mode < 0 || n <= 0)
		return;
	if (n > POINTS_MAX)
		n = POINTS_MAX;
	for (i = 0; i < n; i++)
	{
		memcpy(tpx_buffer_s16_get_pos(b, i), p[i].pos, 3 * sizeof(int16_t));
		*tpx_buffer_s16_get_size(b, i) = (int8_t)p[i].size;
		memcpy(tpx_buffer_s16_get_rgba(b, i), p[i].rgba, 4);
	}
	/* TinyPX draws pairs: pad an odd count with a zero-size point */
	if (n & 1)
	{
		memset(tpx_buffer_s16_get_pos(b, n), 0, 3 * sizeof(int16_t));
		*tpx_buffer_s16_get_size(b, n) = 0;
		n++;
	}
	rdpq_mode_push();
	tpx_state_from_t3d();
	tpx_matrix_push(mat);
	tpx_state_set_base_size(32);
	tpx_state_set_scale(0.18f, 0.18f);
	tpx_particle_draw_s16(b, (uint32_t)n);
	tpx_matrix_pop(1);
	rdpq_mode_pop();
	cur.calls[mode]++;
	cur.points += (uint32_t)n;
}

void gpu_fx_stats(gpu_fx_stats_t *s)
{
	*s = done;
}
