/* SPDX-License-Identifier: MIT */
/*
 * gpu.c -- the renderer's GPU seam on the cart (engine/render/gpu.h):
 * tiny3d / rdpq submission of the world's meshlets.
 *
 * Split from the old tree's n64/port/r64_sector.c per its audit entry
 * (D9; CLEAN ranges): the tiny3d index re-encode and self-check
 * (196-217, 256-285), the prebuilt raw RDP bind list for a CI4 + TLUT
 * page and its rdpq_tex fallback (1453-1538), the strip submit with its
 * per-meshlet sync (1592-1600), and the view / mode setup that the old
 * world pass made before it (camera at the origin, rotation-only view;
 * TEX_SHADE, RGBA16 TLUT, bilinear, perspective, Z on, front-face cull for
 * the BSP face order). Quake bindings (cvars, Con_*, Sys_Error, the old
 * profiler) are gone. Night vision (gpu_world_state, architecture.md
 * D12) is new here.
 */
#include <string.h>

#include <libdragon.h>
#include <rdpq_tex.h>
#include <t3d/t3d.h>

#include "hl64/limits.h"
#include "hl64/settings.h"
#include "engine/core/sys.h"
#include "engine/render/addblend.h"
#include "engine/render/gpu.h"
#include "engine/world/wld.h"
#include "plat/n64/n64.h"

_Static_assert(sizeof(w64_vert_t) == sizeof(T3DVertPacked), "w64_vert_t is T3DVertPacked");

/* the world matrix + its translated movers, per open layer */
#define MATS_PER_FRAME	(HL64_WORLD_LAYERS * (HL64_REN_MOVERS_MAX + 1))

/* The view model's share of the depth range, from its front: the gun
 * drawn after the world never goes into a wall it stands against
 * (GoldSrc's view model depth range 0..0.3, provenance-audit.md
 * r64_alias.c 1304-1413 behaviour facts). With the projection's NDC depth
 * d mapped to d' = VIEW_DEPTH (d + 1) - 1, world surfaces cover it only
 * nearer than ZNEAR / (1 - VIEW_DEPTH) = 5.7 u of eye depth: inside the
 * ducked hull's 6 u above the eye. */
#define VIEW_DEPTH	0.3f

/* per-page raw RDP bind list, kept in the layer's page state */
#define RDL_CMDS	11
#define RDL_TLUT_TMEM	0x800	/* rdpq_tex.c TMEM_PALETTE_ADDR */
#define RDL(id, w0, w1)	((((uint64_t)(((uint32_t)(id) << 24) | (w0))) << 32) | (uint32_t)(w1))

typedef struct {
	uint64_t	cmd[RDL_CMDS];
} page_rdl_t;
_Static_assert(sizeof(page_rdl_t) <= WLD_PAGE_STATE, "page state");

static T3DViewport	vp;
static int		vp_ready;
static T3DMat4FP	*mats;		/* 2 x MATS_PER_FRAME, uncached */
static T3DMat4FP	*proj;		/* 2 x (world, view model), uncached */
static int		mat_frame, mat_n;
static int		fence = -1;	/* alpha compare threshold set (0 off), -1 unknown */
static int		fence_ref = GPU_FENCE_REF;	/* gpu_fence_ref */
static gpu_stats_t	cur, done;
static uint8_t		ramp_tb[4];	/* gpu_page_ramp tint + blend */
static int		ramp_on;
static int		nvg;		/* this frame: night vision (gpu_view_t.nvg) */
static int		nvg_range;	/* the tiny3d fog range is set */

/* ---- strip index encoding ------------------------------------------------- */

int gpu_index_check(unsigned stride, unsigned base)
{
	int16_t probe[3] __attribute__((aligned(8))) = { (int16_t)(0x8000 | 5), 6, 7 };
	uint16_t e0 = (uint16_t)((5 * stride + base) | W64_IDX_RESTART);
	uint16_t e1 = (uint16_t)(6 * stride + base);
	uint16_t e2 = (uint16_t)((7 * stride + base) | W64_IDX_LAST);

	t3d_indexbuffer_convert(probe, 3);
	return !((uint16_t)probe[0] == e0 && (uint16_t)probe[1] == e1 && (uint16_t)probe[2] == e2);
}

void gpu_index_reencode(int16_t *idx, int n, unsigned stride, unsigned base)
{
	int k;

	for (k = 0; k < n; k++)
	{
		uint16_t e = (uint16_t)idx[k];

		idx[k] = (int16_t)((((e & 0x3FFF) - base) / stride) | (e & W64_IDX_RESTART));
	}
	t3d_indexbuffer_convert(idx, n);
}

/* ---- frame ------------------------------------------------------------------ */

void gpu_begin(const gpu_view_t *v)
{
	T3DVec3 eye = {{ 0, 0, 0 }};
	T3DVec3 tgt = {{ v->fwd[0], v->fwd[1], v->fwd[2] }};
	T3DVec3 up = {{ v->up[0], v->up[1], v->up[2] }};
	T3DMat4 pv;
	int c;

	if (!vp_ready)
	{
		vp = t3d_viewport_create_buffered(2);
		mats = malloc_uncached(2 * MATS_PER_FRAME * sizeof(T3DMat4FP));
		proj = malloc_uncached(2 * 2 * sizeof(T3DMat4FP));
		if (!mats || !proj)
			sys_error("gpu: no uncached memory for matrices");
		vp_ready = 1;
	}
	mat_frame ^= 1;
	mat_n = 0;
	memset(&cur, 0, sizeof(cur));
	t3d_frame_start();
	t3d_viewport_set_area(&vp, 0, 0, 320, 240);
	t3d_viewport_set_projection(&vp, T3D_DEG_TO_RAD(v->fov_y), v->znear, v->zfar);
	t3d_viewport_look_at(&vp, &eye, &tgt, &up);
	t3d_viewport_attach(&vp);
	/* the view model's projection: clip z' = k z + (k - 1) w (rows of the
	 * column-major matrix), the same w, x, y and near plane */
	pv = vp.matProj;
	for (c = 0; c < 4; c++)
		pv.m[c][2] = VIEW_DEPTH * pv.m[c][2] + (VIEW_DEPTH - 1) * pv.m[c][3];
	t3d_mat4_to_fixed(&proj[2 * mat_frame], &vp.matProj);
	t3d_mat4_to_fixed(&proj[2 * mat_frame + 1], &pv);
	/* the fog writes vertex alpha in every tiny3d pass while on: off
	 * again on the first lit frame (gpu_world_state turns it on) */
	if (nvg && !v->nvg)
		t3d_fog_set_enabled(false);
	nvg = v->nvg;
	if (nvg && !nvg_range)
	{
		t3d_fog_set_range(GPU_NVG_NEAR, GPU_NVG_FAR);
		nvg_range = 1;
	}
	rdpq_set_mode_standard();
	rdpq_mode_dithering(N64_DITHER_MODE());
	rdpq_mode_tlut(TLUT_RGBA16);
	rdpq_mode_filter(FILTER_BILINEAR);
	rdpq_mode_persp(true);
	rdpq_mode_zbuf(true, true);
	fence = -1;
	fence_ref = GPU_FENCE_REF;
	gpu_world_state();
}

int gpu_nvg(void)
{
	return nvg;
}

void gpu_world_state(void)
{
	if (nvg)
	{
		rdpq_mode_combiner(N64_CC_NVG);
		rdpq_set_prim_color(RGBA32(GPU_NVG_PRIM_R, GPU_NVG_PRIM_G, GPU_NVG_PRIM_B, 255));
		rdpq_set_env_color(RGBA32(GPU_NVG_ENV_R, GPU_NVG_ENV_G, GPU_NVG_ENV_B, 255));
		rdpq_set_fog_color(RGBA32(0, 0, 0, 255));
		rdpq_mode_fog(RDPQ_FOG_STANDARD);
		t3d_fog_set_enabled(true);
	}
	else
		rdpq_mode_combiner(RDPQ_COMBINER_TEX_SHADE);
	t3d_state_set_drawflags(T3D_FLAG_SHADED | T3D_FLAG_TEXTURED | T3D_FLAG_DEPTH
		| T3D_FLAG_NO_LIGHT | N64_CULL);
}

void gpu_view_proj(int view)
{
	t3d_matrix_set_proj(&proj[2 * mat_frame + (view != 0)]);
}

void gpu_end(void)
{
	done = cur;
}

void gpu_matrix_push(const ren_mat_t *m)
{
	T3DMat4FP *f;

	if (mat_n >= MATS_PER_FRAME)
		sys_error("gpu: more than %d matrices in a frame", MATS_PER_FRAME);
	f = &mats[mat_frame * MATS_PER_FRAME + mat_n++];
	t3d_mat4_to_fixed_3x4(f, (const T3DMat4 *)m);
	t3d_matrix_push(f);
}

void gpu_matrix_pop(void)
{
	t3d_matrix_pop(1);
}

/* ---- texture pages ---------------------------------------------------------- */

/* CI4 w x h, power-of-two sides, TMEM line 8..64 bytes (LOAD_BLOCK exact). */
static int rdl_shape(int w, int h)
{
	return w >= 16 && w <= 128 && h >= 1 && !(w & (w - 1)) && !(h & (h - 1)) && w * h <= 4096;
}

/* rdpq_tex_upload_tlut(tlut, 0, 16) + rdpq_tex_upload(TILE0, CI4, REPEAT_INFINITE). */
static void rdl_build(page_rdl_t *l, const w64_texp_t *p)
{
	uint32_t tlut = PhysicalAddr((const uint8_t *)p + sizeof(w64_texp_t));
	uint32_t ci4 = PhysicalAddr((const uint8_t *)p + p->ci4_ofs);
	int w = p->w, h = p->h, words = (w >> 1) >> 3;

	l->cmd[0] = RDL(0x27, 0, 0);		/* SYNC_PIPE */
	l->cmd[1] = RDL(0x31, 0, 0);		/* SYNC_LOAD */
	l->cmd[2] = RDL(0x28, 0, 0);		/* SYNC_TILE */
	l->cmd[3] = RDL(0x3D, (FMT_RGBA16 << 19) | (256 + 4 - 1), tlut);
	l->cmd[4] = RDL(0x35, (FMT_I4 << 19) | (((256 + 8) / 8) << 9) | (RDL_TLUT_TMEM / 8),
		(RDPQ_TILE_INTERNAL << 24) | (1 << 19) | (1 << 9));
	l->cmd[5] = RDL(0x30, 0, (RDPQ_TILE_INTERNAL << 24) | (15 << 14));	/* LOAD_TLUT 16 */
	l->cmd[6] = RDL(0x3D, (FMT_RGBA16 << 19) | ((w >> 2) - 1), ci4);
	l->cmd[7] = RDL(0x35, FMT_RGBA16 << 19, (TILE1 << 24) | (1 << 19) | (1 << 9));
	l->cmd[8] = RDL(0x35, (FMT_CI4 << 19) | (words << 9),
		(TILE0 << 24) | ((h == 1) << 19) | (__builtin_ctz(h) << 14) | (__builtin_ctz(w) << 4));
	l->cmd[9] = RDL(0x33, 0, (TILE1 << 24) | ((((w * h) >> 2) - 1) << 12)
		| ((2048 + words - 1) / words));		/* LOAD_BLOCK */
	l->cmd[10] = RDL(0x32, 0, (TILE0 << 24) | ((w * 4 - 4) << 12) | (h * 4 - 4));
	data_cache_hit_writeback(l, sizeof(*l));
}

/* rdpq internal autosync (rdpq.h), also used by tiny3d */
extern void __rdpq_autosync_use(uint32_t res);

void gpu_bind_page(const w64_texp_t *p, uint8_t *state)
{
	int f = (p->flags & W64_TEXF_FENCE) ? fence_ref : 0;

	if (rdl_shape(p->w, p->h))
	{
		page_rdl_t *l = (page_rdl_t *)state;

		/* rebuilt when the page landed elsewhere in the pool */
		if ((uint32_t)l->cmd[6] != PhysicalAddr((const uint8_t *)p + p->ci4_ofs))
			rdl_build(l, p);
		rdpq_exec(l->cmd, sizeof(l->cmd));
		/* the list's loads and tiles are invisible to autosync: the next
		 * rdpq tile / TMEM / mode change must sync first */
		__rdpq_autosync_use(AUTOSYNC_TILES | AUTOSYNC_TMEMS | AUTOSYNC_PIPE);
	}
	else
	{
		surface_t surf = surface_make((void *)((const uint8_t *)p + p->ci4_ofs), FMT_CI4,
			p->w, p->h, p->w / 2);

		rdpq_tex_upload_tlut((uint16_t *)((const uint8_t *)p + sizeof(w64_texp_t)), 0, 16);
		rdpq_tex_upload(TILE0, &surf, &(rdpq_texparms_t){
			.s.repeats = REPEAT_INFINITE, .t.repeats = REPEAT_INFINITE });
	}
	if (f != fence)
	{
		fence = f;
		rdpq_mode_alphacompare(f);
	}
	if (ramp_on)
	{
		/* additive page: its cooked ramp TLUT over the RGBA16 one, its ends */
		const add_ramp_t *r = add_page_ramp(p);
		uint8_t prim[4] = { 0, 0, 0, 0 }, env[4] = { 0, 0, 0, 0 };

		if (r)
		{
			rdpq_tex_upload_tlut((uint16_t *)r->ia, 0, 16);
			add_ramp_ends(r, ramp_tb, ramp_tb[3], prim, env);
		}
		rdpq_set_prim_color(RGBA32(prim[0], prim[1], prim[2], prim[3]));
		rdpq_set_env_color(RGBA32(env[0], env[1], env[2], env[3]));
	}
	cur.binds++;
}

void gpu_page_ramp(const uint8_t tint_blend[4])
{
	ramp_on = tint_blend != NULL;
	if (ramp_on)
		memcpy(ramp_tb, tint_blend, 4);
}

void gpu_state_lost(void)
{
	fence = -1;
}

void gpu_fence_ref(int ref)
{
	ref = ref < 1 ? 1 : ref > 255 ? 255 : ref;
	if (ref == fence_ref)
		return;
	fence_ref = ref;
	if (fence > 0)
	{
		fence = ref;
		rdpq_mode_alphacompare(ref);
	}
}

/* ---- meshlets --------------------------------------------------------------- */

void gpu_draw(const w64_vert_t *v, int nv, const int16_t *idx, int ni)
{
	t3d_vert_load((const T3DVertPacked *)v, 0, (uint32_t)nv);
	/* sync before the next load reuses the vertex cache: without it the
	 * RSP corrupts DMEM (rspq "Invalid overlay" within seconds) */
	t3d_tri_draw_strip_and_sync((int16_t *)idx, ni);
	cur.meshlets++;
	cur.verts += (uint32_t)nv;
}

void gpu_writeback(const void *p, uint32_t n)
{
	data_cache_hit_writeback(p, n);
}

void gpu_stats(gpu_stats_t *s)
{
	*s = done;
}
