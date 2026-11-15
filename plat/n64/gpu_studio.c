/* SPDX-License-Identifier: MIT */
/*
 * gpu_studio.c -- the studio renderer's GPU seam on the cart
 * (engine/render/gpu_studio.h): tiny3d / rdpq submission of the cooked
 * bone-local meshlets (docs/formats/studio-format.md "Draw mesh").
 *
 * After the old tree's STB2 world draw (n64/port/r64_studio_bake.c,
 * provenance-audit.md entry: CLEAN ranges sk_step 1295-1476 and sk_draw
 * 1478-1665, the T3D skinned-model scheme): each run is recorded once into
 * an rspq block -- per meshlet, per bone run one t3d_matrix_set of the
 * slot's matrix through a segment placeholder (multiplied with the
 * entity matrix) and one t3d_vert_load, then the triangles and a sync.
 * The cook already packs, sorts and pads the vertices, so recording is one
 * linear pass and needs no CPU-side vertex work. Per entity: the bone
 * matrices go into this frame's half of the matrix pool, the segment
 * points at them, each run binds its skin and replays its block. Quake
 * bindings of the old file (entity_t, Mod_*, Con_*) are gone; chrome
 * (old 828-879) is not used here: the world draw keeps the cooked texture
 * coordinates, the view model's chrome is CPU work in game/studio.
 */
#include <string.h>

#include <libdragon.h>
#include <t3d/t3d.h>
#include <t3d/t3dmath.h>

#include "engine/core/sys.h"
#include "engine/render/gpu_studio.h"
#include "engine/world/wld.h"
#include "plat/n64/n64.h"

_Static_assert(sizeof(w64_vert_t) == sizeof(T3DVertPacked), "w64_vert_t is T3DVertPacked");

#define SEG_BONES	6

typedef struct {
	int		nruns;
	rspq_block_t	**blocks;	/* [nruns] */
	uint8_t		*page_state;	/* [ntex] x WLD_PAGE_STATE */
	rspq_block_t	*cpu_blocks;	/* unused (CPU path draws immediately) */
} model_state_t;

static T3DMat4FP	*pool;		/* uncached, 2 halves */
static int		pool_half, pool_used;
static int		frame_flip;
static gpu_studio_stats_t cur, done;

int gpu_studio_init(void *mem, uint32_t bytes)
{
	pool = (T3DMat4FP *)UncachedAddr(mem);
	pool_half = (int)(bytes / sizeof(T3DMat4FP) / 2);
	return pool_half;
}

uint32_t gpu_studio_state_size(const sfmt_mesh_t *mesh)
{
	return (uint32_t)(sizeof(model_state_t) + mesh->nruns * sizeof(rspq_block_t *)
		+ mesh->ntex * WLD_PAGE_STATE + 16);
}

static const uint8_t *at(const sfmt_mesh_t *m, uint32_t ofs)
{
	return (const uint8_t *)m + ofs;
}

int gpu_studio_build(void *state, const sfmt_mesh_t *mesh)
{
	model_state_t *s = state;
	const sfmt_run_t *runs = (const sfmt_run_t *)at(mesh, mesh->run_ofs);
	const sfmt_meshlet_t *ml = (const sfmt_meshlet_t *)at(mesh, mesh->meshlet_ofs);
	const sfmt_load_t *ld = (const sfmt_load_t *)at(mesh, mesh->load_ofs);
	const T3DVertPacked *verts = (const T3DVertPacked *)at(mesh, mesh->vert_ofs);
	const uint8_t *tris = at(mesh, mesh->tri_ofs);
	int r, k, l, t, slot = 0;

	memset(s, 0, gpu_studio_state_size(mesh));
	s->nruns = mesh->nruns;
	s->blocks = (rspq_block_t **)(s + 1);
	s->page_state = (uint8_t *)(((uintptr_t)(s->blocks + mesh->nruns) + 15) & ~(uintptr_t)15);
	/* the vertex table was DMA'd from the cart: no CPU lines to write back */
	for (r = 0; r < mesh->nruns; r++)
	{
		const sfmt_run_t *ru = &runs[r];
		int cur_bone = -1;

		rspq_block_begin();
		for (k = ru->meshlet_first; k < ru->meshlet_first + ru->meshlet_count; k++)
		{
			const sfmt_meshlet_t *m = &ml[k];

			for (l = m->load_first; l < m->load_first + m->nloads; l++)
			{
				if (ld[l].bslot != cur_bone)
				{
					cur_bone = ld[l].bslot;
					t3d_matrix_set((const T3DMat4FP *)t3d_segment_placeholder(SEG_BONES)
						+ cur_bone, true);
				}
				t3d_vert_load(&verts[slot >> 1], ld[l].cache, ld[l].count);
				slot += ld[l].count + (ld[l].count & 1);
			}
			for (t = m->tri_first; t < m->tri_first + m->ntris; t++)
				t3d_tri_draw(tris[3 * t], tris[3 * t + 1], tris[3 * t + 2]);
			t3d_tri_sync();
		}
		s->blocks[r] = rspq_block_end();
	}
	return 0;
}

void gpu_studio_release(void *state)
{
	model_state_t *s = state;
	int r;

	for (r = 0; r < s->nruns; r++)
		if (s->blocks[r])
			rspq_block_free(s->blocks[r]);
	s->nruns = 0;
}

static void to_t3d(T3DMat4 *o, const float b[3][4])
{
	int r, c;

	for (c = 0; c < 4; c++)
	{
		for (r = 0; r < 3; r++)
			o->m[c][r] = b[r][c];
		o->m[c][3] = c == 3 ? 1.0f : 0.0f;
	}
}

void gpu_studio_frame(void)
{
	frame_flip ^= 1;
	pool_used = 0;
	memset(&cur, 0, sizeof(cur));
	done = cur;
}

#define CC_TEX_PRIM	RDPQ_COMBINER1((TEX0, 0, PRIM, 0), (TEX0, 0, PRIM, 0))
/* render-fx.md 2a: rgb = ENV + (PRIM - ENV) x I, a = A x PRIM alpha */
#define CC_RAMP		RDPQ_COMBINER1((PRIM, ENV, TEX0, ENV), (TEX0, 0, PRIM, 0))

static int	ramp;		/* the ramp combiner + IA16 TLUT are set */

void gpu_studio_begin(void)
{
	rdpq_mode_combiner(CC_TEX_PRIM);
	rdpq_mode_tlut(TLUT_RGBA16);
	ramp = 0;
	t3d_state_set_drawflags(T3D_FLAG_TEXTURED | T3D_FLAG_DEPTH | T3D_FLAG_NO_LIGHT | N64_CULL);
}

void gpu_studio_end(void)
{
	/* the world's state (gpu_begin) back for later passes */
	gpu_page_ramp(NULL);
	rdpq_mode_combiner(RDPQ_COMBINER_TEX_SHADE);
	if (ramp)
		rdpq_mode_tlut(TLUT_RGBA16);
	ramp = 0;
	rdpq_mode_blender(0);
	gpu_fence_ref(GPU_FENCE_REF);
	rdpq_mode_zbuf(true, true);
	t3d_state_set_drawflags(T3D_FLAG_SHADED | T3D_FLAG_TEXTURED | T3D_FLAG_DEPTH
		| T3D_FLAG_NO_LIGHT | N64_CULL);
	done = cur;
}

static void set_mode(const gpu_studio_ent_t *e)
{
	int add = e->mode == GPU_STUDIO_ADD;

	/* additive: each skin page binds its ramp TLUT and sets PRIM / ENV to
	 * its ends, tinted by rgba (render-fx.md 2a) */
	gpu_page_ramp(add ? e->rgba : NULL);
	if (add != ramp)
	{
		rdpq_mode_combiner(add ? CC_RAMP : CC_TEX_PRIM);
		rdpq_mode_tlut(add ? TLUT_IA16 : TLUT_RGBA16);
		ramp = add;
	}
	if (!add)
		rdpq_set_prim_color(RGBA32(e->rgba[0], e->rgba[1], e->rgba[2], e->rgba[3]));
	/* masked skins: half coverage of TEX x PRIM alpha; the additive ramp
	 * scales the texel alpha by its blend: 1 as before */
	gpu_fence_ref(add ? 1 : (e->rgba[3] + 1) / 2);
	switch (e->mode)
	{
	case GPU_STUDIO_BLEND:
	case GPU_STUDIO_ADD:
		rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
		rdpq_mode_zbuf(true, false);
		break;
	default:
		rdpq_mode_blender(0);
		rdpq_mode_zbuf(true, true);
		break;
	}
}

static void bind(model_state_t *s, const sfmt_mesh_t *mesh, int tex)
{
	const uint32_t *tofs = (const uint32_t *)at(mesh, mesh->tex_ofs);

	gpu_bind_page((const w64_texp_t *)at(mesh, tofs[tex]),
		s->page_state + (uint32_t)tex * WLD_PAGE_STATE);
}

int gpu_studio_draw(void *state, const sfmt_mesh_t *mesh, const gpu_studio_ent_t *e)
{
	model_state_t *s = state;
	T3DMat4FP *mats;
	T3DMat4 m;
	int k;

	if (pool_used + 1 + e->nbones > pool_half)
		return -1;
	mats = &pool[frame_flip * pool_half + pool_used];
	pool_used += 1 + e->nbones;
	t3d_mat4_to_fixed_3x4(&mats[0], (const T3DMat4 *)&e->model);
	for (k = 0; k < e->nbones; k++)
	{
		to_t3d(&m, e->bones[k]);
		t3d_mat4_to_fixed_3x4(&mats[1 + k], &m);
	}
	set_mode(e);
	/* the view model's depth range (each MVP is formed at its push) */
	if (e->view)
		gpu_view_proj(1);
	t3d_matrix_push(&mats[0]);
	t3d_segment_set(SEG_BONES, &mats[1]);
	t3d_matrix_push_pos(1);
	for (k = 0; k < e->nruns; k++)
	{
		bind(s, mesh, e->tex[k]);
		rspq_block_run(s->blocks[e->runs[k]]);
	}
	if (e->view)
		gpu_view_proj(0);
	t3d_matrix_pop(2);
	cur.ents++;
	cur.runs += (uint32_t)e->nruns;
	cur.mats += (uint32_t)(1 + e->nbones);
	return 0;
}

int gpu_studio_draw_cpu(void *state, const sfmt_mesh_t *mesh, const w64_vert_t *verts,
	const gpu_studio_ent_t *e)
{
	model_state_t *s = state;
	const sfmt_run_t *runs = (const sfmt_run_t *)at(mesh, mesh->run_ofs);
	const sfmt_meshlet_t *ml = (const sfmt_meshlet_t *)at(mesh, mesh->meshlet_ofs);
	const sfmt_load_t *ld = (const sfmt_load_t *)at(mesh, mesh->load_ofs);
	const uint8_t *tris = at(mesh, mesh->tri_ofs);
	const T3DVertPacked *v = (const T3DVertPacked *)verts;
	T3DMat4FP *mats;
	int k, i, l, t;

	if (pool_used + 1 > pool_half)
		return -1;
	mats = &pool[frame_flip * pool_half + pool_used++];
	t3d_mat4_to_fixed_3x4(mats, (const T3DMat4 *)&e->model);
	set_mode(e);
	/* the view model's depth range (the MVP is formed at the push) */
	if (e->view)
		gpu_view_proj(1);
	t3d_matrix_push(mats);
	for (k = 0; k < e->nruns; k++)
	{
		const sfmt_run_t *ru = &runs[e->runs[k]];
		int slot = 0;

		/* the run's first slot: every earlier load's padded count */
		for (i = 0; i < ru->meshlet_first; i++)
			for (l = ml[i].load_first; l < ml[i].load_first + ml[i].nloads; l++)
				slot += ld[l].count + (ld[l].count & 1);
		bind(s, mesh, e->tex[k]);
		for (i = ru->meshlet_first; i < ru->meshlet_first + ru->meshlet_count; i++)
		{
			for (l = ml[i].load_first; l < ml[i].load_first + ml[i].nloads; l++)
			{
				t3d_vert_load(&v[slot >> 1], ld[l].cache, ld[l].count);
				slot += ld[l].count + (ld[l].count & 1);
			}
			for (t = ml[i].tri_first; t < ml[i].tri_first + ml[i].ntris; t++)
				t3d_tri_draw(tris[3 * t], tris[3 * t + 1], tris[3 * t + 2]);
			t3d_tri_sync();
		}
	}
	if (e->view)
		gpu_view_proj(0);
	t3d_matrix_pop(1);
	cur.ents++;
	cur.runs += (uint32_t)e->nruns;
	cur.mats++;
	return 0;
}

void gpu_studio_stats(gpu_studio_stats_t *s)
{
	*s = done;
}
