/* SPDX-License-Identifier: MIT */
/*
 * world.c -- the sector renderer (docs/architecture.md 9.2-9.3,
 * docs/formats/w64-format.md "Runtime requirements"): eye leaf -> cooked
 * visible-sector list -> sector bbox and meshlet bbox against the four
 * frustum planes -> counting sort by texture page -> per page one bind,
 * per meshlet one vertex load + one pre-encoded strip.
 *
 * Split from the old tree's n64/port/r64_sector.c per its audit entry
 * (D9; CLEAN ranges: frustum planes and p-vertex box cull 386-429, the
 * draw-list collection and counting sort 1539-1613 / 1614-1773, the
 * recolour of styled meshlets 515-551, the scratch-copy structure of
 * the dynamic-light pass 581-633 / 650-662). Its Quake bindings
 * (entity_t, cl_*, r_refdef, d_lightstylevalue, cvars, Con_*) are gone:
 * the view is ren_cam, movers come from the brush entities' components
 * (M3; the layer's ENTS placements while no brush entity exists), style
 * values from engine/world. The flagged ranges are rewritten from the
 * specs: texture animation (431-453) from world-surfaces.md 4, the
 * dynamic-light falloff (634-649) from dlights.md 3. tiny3d / rdpq calls
 * live behind gpu.h. The flashlight is the pool pass over the drawn
 * list (dlights.md 3; world list only, movers not covered yet). OPEN
 * seams (cwg-format.md): while the player is in an OPEN seam's approach
 * box, B's draw set is drawn across layers (draw_open_seam).
 *
 * Movers at rest at the map origin are world-space geometry: they join
 * the world list (one page sort, per-meshlet cull, no matrix). Movers
 * with an origin or angles draw their own list under their matrix;
 * translucent render modes draw in the translucent pass
 * (engine/render/trans.c, render-fx.md 2).
 */
#include <math.h>
#include <string.h>

#include "hl64/ent.h"
#include "hl64/limits.h"
#include "hl64/settings.h"
#include "hl64/studio.h"
#include "hl64/seam.h"
#include "engine/core/log.h"
#include "engine/core/sys.h"
#include "engine/phys/player.h"
#include "engine/mem/carve.h"
#include "engine/render/gpu_fx.h"
#include "engine/render/ren.h"
#include "engine/render/trans.h"
#include "engine/world/wld.h"

#define ZNEAR		4.0f
#define ZFAR		8192.0f
#define DL_PAIRS	512		/* dlit scratch per frame (vertex pairs) */
#define FP_PAIRS	192		/* flashlight pool vertex pairs per frame */
#define FP_IDX		512		/* flashlight pool strip indices per frame */
#define FP_IDX_ALIGN	4		/* indices per 8 bytes: each batch's strip start (fp_flush) */
_Static_assert(FP_IDX % FP_IDX_ALIGN == 0, "padded batch starts stay inside the pool");

typedef struct {
	const w64_meshlet_t	*m;
	uint8_t			*chunk;
	uint16_t		chunk_idx;
	uint16_t		page;
} vis_t;

static vis_t		*vis;		/* HL64_REN_VIS_MAX, scratch region */
static uint16_t		*order;
static uint16_t		page_count[HL64_REN_PAGES_MAX + 1], page_pos[HL64_REN_PAGES_MAX + 1];
static uint16_t		tex_page0[HL64_REN_TEX_MAX];	/* per texture: page, entity frame 0 */
static const void	*last_bind;
static float		fr_n[4][3];	/* inward frustum plane normals (n . p >= n . eye) */
static float		eye_g[3];	/* eye, global */
static uint32_t		r10;		/* floor(game time x 10) */
static ren_stats_t	st, last;
/* this frame's OPEN-seam B layer (ren_open_layer): slot, -T, carry box
 * in B's frame; slot -1 none */
static struct {
	int		slot;
	float		off[3], lo[3], hi[3];
} oseam = { -1, { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 } };

static w64_vert_t	*dl_buf[2];
static w64_vert_t	*fp_v[2];	/* flashlight pool, double-buffered */
static int16_t		*fp_i[2];
static int		dl_used, dl_cur;
static int		ndl;
static const ren_dlight_t *dl[HL64_DLIGHTS];
static float		norm5[32], norm6[64];

void ren_init(void)
{
	uint8_t *s = mem_region(MEM_SCRATCH);
	uint32_t need = HL64_REN_VIS_MAX * (uint32_t)(sizeof(vis_t) + sizeof(uint16_t));
	int i;

	if (need > mem_region_bytes(MEM_SCRATCH))
		sys_error("render: draw list %lu B > scratch region", (unsigned long)need);
	vis = (vis_t *)s;
	order = (uint16_t *)(s + HL64_REN_VIS_MAX * sizeof(vis_t));
	if (2u * DL_PAIRS * sizeof(w64_vert_t) + 2u * FP_PAIRS * sizeof(w64_vert_t)
		+ 2u * FP_IDX * sizeof(int16_t) > mem_region_bytes(MEM_DLIGHT))
		sys_error("render: dlight scratch + flashlight pool > dlight region");
	dl_buf[0] = mem_region(MEM_DLIGHT);
	dl_buf[1] = dl_buf[0] + DL_PAIRS;
	fp_v[0] = dl_buf[1] + DL_PAIRS;
	fp_v[1] = fp_v[0] + FP_PAIRS;
	fp_i[0] = (int16_t *)(fp_v[1] + FP_PAIRS);
	fp_i[1] = fp_i[0] + FP_IDX;
	for (i = 0; i < 32; i++)
		norm5[i] = (float)(i - 16) / 15.5f;
	for (i = 0; i < 64; i++)
		norm6[i] = (float)(i - 32) / 31.5f;
}

/* ---- camera maths ----------------------------------------------------- */

void ren_angle_axes(const float ang[3], float f[3], float r[3], float u[3])
{
	const float k = 3.14159265358979f / 180.0f;
	float sp = sinf(ang[0] * k), cp = cosf(ang[0] * k);
	float sy = sinf(ang[1] * k), cy = cosf(ang[1] * k);
	float sr = sinf(ang[2] * k), cr = cosf(ang[2] * k);

	f[0] = cp * cy;
	f[1] = cp * sy;
	f[2] = -sp;
	r[0] = -sr * sp * cy + cr * sy;
	r[1] = -sr * sp * sy - cr * cy;
	r[2] = -sr * cp;
	u[0] = cr * sp * cy + sr * sy;
	u[1] = cr * sp * sy - sr * cy;
	u[2] = cr * cp;
}

/* 4:3 picture: tan(fov_y / 2) = tan(fov_x / 2) x 3 / 4 */
float ren_fov_y(float fov_x)
{
	const float k = 3.14159265358979f / 360.0f;

	return atanf(tanf(fov_x * k) * 0.75f) / k;
}

/* Box (mins, maxs) fully outside one plane: n . p-vertex < d. */
static int box_culled(const int16_t *bb, const float *d)
{
	int i;

	for (i = 0; i < 4; i++)
	{
		const float *n = fr_n[i];
		float v = n[0] * (n[0] >= 0 ? bb[3] : bb[0])
			+ n[1] * (n[1] >= 0 ? bb[4] : bb[1])
			+ n[2] * (n[2] >= 0 ? bb[5] : bb[2]);

		if (v < d[i])
			return 1;
	}
	return 0;
}

/* Plane distances for geometry whose local origin sits at org (global). */
static void plane_d(const float org[3], float d[4])
{
	float e[3];
	int i;

	for (i = 0; i < 3; i++)
		e[i] = eye_g[i] - org[i];
	for (i = 0; i < 4; i++)
		d[i] = fr_n[i][0] * e[0] + fr_n[i][1] * e[1] + fr_n[i][2] * e[2];
}

/* ---- texture animation (world-surfaces.md 4) ------------------------- */

/* Texture shown for texture t with entity frame `frame` at r10 =
 * floor(time x 10): the alternate sequence when frame != 0 and one
 * exists; then the sequence's frame whose [anim_min, anim_max) holds
 * r10 mod anim_total. */
/* (declared in ren.h) */
int ren_tex_anim(const w64_tex_t *texs, int ntex, int t, int frame, uint32_t r)
{
	const w64_tex_t *x = &texs[t];
	int start, steps;
	uint32_t k;

	if (!(x->flags & W64_TEXF_ANIM))
		return t;
	if (frame && x->alternate != W64_NONE)
		t = x->alternate;
	x = &texs[t];
	if (!x->anim_total)
		return t;
	k = r % x->anim_total;
	start = t;
	for (steps = 0; steps < ntex; steps++)
	{
		if (x->anim_min <= k && k < x->anim_max)
			return t;
		t = x->anim_next;
		x = &texs[t];
	}
	return start;	/* not reached for a cooked (validated) sequence */
}

/* ---- styled meshlets (lightstyles.md 3) --------------------------------- */

static void relight(wld_layer_t *L, int c, const uint8_t *chunk, const w64_meshlet_t *m)
{
	const w64_rend_t *r = (const w64_rend_t *)chunk;
	const w64_style_t *s = (const w64_style_t *)(chunk + 24 + 24 * r->nmeshlets) + m->style;
	uint32_t *done = &L->style_epoch[L->ch[c].style_base + m->style];
	int slot[4], val[4], ns, k, i, nv = W64_MESHLET_NV(m), need = *done == 0;
	uint32_t ls = L->hdr.lightscale_x100;
	const uint8_t *lum;
	w64_vert_t *v;

	for (ns = 0; ns < 4 && s->styles[ns] != 255; ns++)
	{
		slot[ns] = wld_style_slot(L, s->styles[ns]);
		if (slot[ns] >= 0 && wld_style_changed(slot[ns]) > *done)
			need = 1;
	}
	if (!need)
		return;
	for (k = 0; k < ns; k++)
		val[k] = slot[k] >= 0 ? wld_style_value(slot[k]) : 256;
	lum = chunk + s->data_ofs;
	v = (w64_vert_t *)(chunk + W64_MESHLET_VOFS(m));
	for (i = 0; i < nv; i++, lum += ns)
	{
		uint32_t l = 0, c8;
		uint32_t *rgba = (i & 1) ? &v[i >> 1].rgbaB : &v[i >> 1].rgbaA;

		for (k = 0; k < ns; k++)
			l += (uint32_t)lum[k] * (uint32_t)val[k];
		c8 = ((l >> 8) * ls + 50) / 100;
		if (c8 > 255)
			c8 = 255;
		*rgba = c8 << 24 | c8 << 16 | c8 << 8 | (*rgba & 0xFF);
	}
	gpu_writeback(v, (uint32_t)((nv + 1) >> 1) * sizeof(w64_vert_t));
	*done = wld_style_epoch();
	st.relit++;
}

/* ---- dynamic lights on the world (dlights.md 3) ------------------------ */

/* Scratch copy of the meshlet lit by the live lights whose sphere meets
 * its box (local frame at org), or the cooked vertices. */
static const w64_vert_t *dlight_verts(const w64_meshlet_t *m, const w64_vert_t *src,
	const float org[3], uint32_t ls)
{
	const ren_dlight_t *hit[HL64_DLIGHTS];
	float lo[HL64_DLIGHTS][3];
	int nh = 0, i, k, nv = W64_MESHLET_NV(m), pairs = (nv + 1) >> 1;
	w64_vert_t *dst;

	for (k = 0; k < ndl; k++)
	{
		float d2 = 0, rad = dl[k]->radius, o[3];

		for (i = 0; i < 3; i++)
		{
			o[i] = dl[k]->origin[i] - org[i];
			float e = o[i] < m->bbox[i] ? m->bbox[i] - o[i]
				: o[i] > m->bbox[i + 3] ? o[i] - m->bbox[i + 3] : 0;
			d2 += e * e;
		}
		if (d2 < rad * rad)
		{
			memcpy(lo[nh], o, sizeof(o));
			hit[nh++] = dl[k];
		}
	}
	if (!nh || dl_used + pairs > DL_PAIRS)
		return src;
	dst = dl_buf[dl_cur] + dl_used;
	dl_used += pairs;
	memcpy(dst, src, (size_t)pairs * sizeof(w64_vert_t));
	for (i = 0; i < nv; i++)
	{
		w64_vert_t *p = &dst[i >> 1];
		const int16_t *pos = (i & 1) ? p->posB : p->posA;
		uint16_t nn = (i & 1) ? p->normB : p->normA;
		uint32_t *rgba = (i & 1) ? &p->rgbaB : &p->rgbaA;
		float n[3], add = 0;
		uint32_t c8;

		/* 5.6.5 two's-complement components; tables are indexed by
		 * value + 16 / + 32 */
		n[0] = norm5[((nn >> 11) & 31) ^ 16];
		n[1] = norm6[((nn >> 5) & 63) ^ 32];
		n[2] = norm5[(nn & 31) ^ 16];
		/* the 5.6.5 quantisation shortens the normal (axial ~0.97):
		 * p must be the true plane distance */
		{
			float len2 = n[0] * n[0] + n[1] * n[1] + n[2] * n[2];

			if (len2 > 0)
			{
				float inv = 1.0f / sqrtf(len2);

				n[0] *= inv;
				n[1] *= inv;
				n[2] *= inv;
			}
		}
		for (k = 0; k < nh; k++)
		{
			float d[3] = { lo[k][0] - pos[0], lo[k][1] - pos[1], lo[k][2] - pos[2] };
			float pd = d[0] * n[0] + d[1] * n[1] + d[2] * n[2];
			float q2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2] - pd * pd;

			add += ren_dlight_falloff(hit[k]->radius, hit[k]->minlight, pd,
				q2 > 0 ? sqrtf(q2) : 0);
		}
		if (add <= 0)
			continue;
		c8 = (*rgba >> 24) + (uint32_t)(add * (float)ls / 100.0f);
		if (c8 > 255)
			c8 = 255;
		*rgba = c8 << 24 | c8 << 16 | c8 << 8 | (*rgba & 0xFF);
	}
	gpu_writeback(dst, (uint32_t)pairs * sizeof(w64_vert_t));
	st.dlit++;
	return dst;
}

/* ---- lists ---------------------------------------------------------------- */

static int add_meshlets(wld_layer_t *L, int n, int c, uint8_t *ch, const float *d)
{
	const w64_rend_t *r = (const w64_rend_t *)ch;
	const w64_meshlet_t *m = (const w64_meshlet_t *)(ch + 24);
	int k, page;

	for (k = 0; k < r->nmeshlets; k++, m++)
	{
		if (box_culled(m->bbox, d))
			continue;
		if (n >= HL64_REN_VIS_MAX)
		{
			st.dropped++;
			continue;
		}
		page = tex_page0[m->tex];
		if (page == W64_NONE)
			continue;
		if (!wld_chunk_use(L, L->dir->texp_first + page))
		{
			st.missing++;
			continue;
		}
		vis[n].m = m;
		vis[n].chunk = ch;
		vis[n].chunk_idx = (uint16_t)c;
		vis[n].page = (uint16_t)page;
		n++;
	}
	return n;
}

/* Counting sort of vis[0, n) by page; submit under the pushed matrix. */
static void draw_list(wld_layer_t *L, int n, const float org[3], int lit)
{
	int i, p, maxp = 0, sum = 0;
	uint32_t ls = L->hdr.lightscale_x100;

	for (i = 0; i < n; i++)
	{
		p = vis[i].page;
		page_count[p]++;
		if (p >= maxp)
			maxp = p + 1;
	}
	for (p = 0; p < maxp; p++)
	{
		page_pos[p] = (uint16_t)sum;
		sum += page_count[p];
		page_count[p] = 0;
	}
	for (i = 0; i < n; i++)
		order[page_pos[vis[i].page]++] = (uint16_t)i;
	for (i = 0; i < n; i++)
	{
		const vis_t *e = &vis[order[i]];
		const w64_meshlet_t *m = e->m;
		const w64_texp_t *tp = L->ch[L->dir->texp_first + e->page].data;
		const w64_vert_t *v;
		const int16_t *idx = (const int16_t *)(e->chunk + W64_MESHLET_IOFS(m));
		int nv = W64_MESHLET_NV(m), ni = W64_MESHLET_NI(m), j, faces = 0;

		if ((const void *)tp != last_bind)
		{
			gpu_bind_page(tp, L->page_state + (uint32_t)e->page * WLD_PAGE_STATE);
			last_bind = tp;
			st.binds++;
		}
		if (m->style != W64_NONE)
			relight(L, e->chunk_idx, e->chunk, m);
		v = (const w64_vert_t *)(e->chunk + W64_MESHLET_VOFS(m));
		if (lit && ndl)
			v = dlight_verts(m, v, org, ls);
		gpu_draw(v, nv, idx, ni);
		for (j = 0; j < ni; j++)
			faces += (idx[j] & W64_IDX_RESTART) != 0;
		st.meshlets++;
		st.verts += (uint32_t)nv;
		st.tris += (uint32_t)(ni - 2 * faces);
	}
}

static void push_translation(const float org[3])
{
	ren_mat_t mt;
	int k;

	memset(&mt, 0, sizeof(mt));
	for (k = 0; k < 3; k++)
	{
		mt.m[k][k] = 1.0f;
		mt.m[3][k] = org[k] - eye_g[k];
	}
	mt.m[3][3] = 1.0f;
	gpu_matrix_push(&mt);
}

/* A mover's placement box (local bbox + origin, map-local) meets the box
 * of a sector the eye leaf sees (architecture.md 5.5: an entity is tested
 * by its overlapped sectors against the visible set). */
static int mover_visible(const wld_layer_t *L, const wld_mover_t *mv, const uint16_t *list,
	int cnt)
{
	const int16_t *mb = L->subs[mv->sub].bbox;
	int i, k;

	for (i = 0; i < cnt; i++)
	{
		const int16_t *sb = L->sects[list[i]].bbox;

		for (k = 0; k < 3; k++)
			if (mb[k] + mv->origin[k] > sb[k + 3] || mb[k + 3] + mv->origin[k] < sb[k])
				break;
		if (k == 3)
			return 1;
	}
	return 0;
}

static int at_origin(const wld_mover_t *mv)
{
	return mv->origin[0] == 0 && mv->origin[1] == 0 && mv->origin[2] == 0;
}

/* ---- movers as entities (M3) --------------------------------------------------------
 *
 * A brush entity whose model is a MOVR submodel of this layer is drawn from
 * its components: origin, angles, render mode (render-fx.md 2, brush
 * column). Normal and TransAlpha draw in the opaque pass (at rest at the
 * map origin they join the world list); the other modes go to the
 * translucent pass (engine/render/trans.c), which calls ren_mover_draw
 * back to front. A layer without brush entities (no game, records not
 * spawned: the M1 free-camera view) draws the ENTS placements instead. */

static uint16_t	mov_ent[HL64_MAX_ENTITIES];
static int	nmov;

static int mover_sub(const wld_layer_t *L, int e)
{
	res_t m = ent_rnd(e)->model;
	int sub;

	if (RES_KIND(m) != RES_K_SUBMODEL || RES_SUB_SLOT(m) != L->slot)
		return -1;
	sub = RES_SUB_NUM(m);
	if (sub >= L->dir->nsub || L->subs[sub].kind != W64_SUB_MOVR)
		return -1;
	return sub;
}

/* Live slots only (the live bitset): a scan of every slot read one
 * ent_id_t cache line per slot, ~280 us a frame on the cart. */
static void collect_movers(const wld_layer_t *L)
{
	int e;

	nmov = 0;
	for (e = ent_next_live(0); e >= 0; e = ent_next_live(e))
		if (ent_idc(e)->layer == L->slot && mover_sub(L, e) >= 0)
			mov_ent[nmov++] = (uint16_t)e;
}

static int rotated(const float a[3])
{
	return a[0] != 0 || a[1] != 0 || a[2] != 0;
}

/* Model matrix of a mover (local -> camera-relative): the collision
 * frame's axes (engine/coll: forward, -right, up), its origin. */
static void mover_matrix(int e, const float org_g[3], ren_mat_t *mt)
{
	float f[3], r[3], u[3];
	int k;

	memset(mt, 0, sizeof(*mt));
	ren_angle_axes(ent_cv(e)->angles, f, r, u);
	for (k = 0; k < 3; k++)
	{
		mt->m[0][k] = f[k];
		mt->m[1][k] = -r[k];
		mt->m[2][k] = u[k];
		mt->m[3][k] = org_g[k] - eye_g[k];
	}
	mt->m[3][3] = 1.0f;
}

/* The entity's linked box (map-local) meets a visible sector and the
 * frustum. */
static int ent_visible(const wld_layer_t *L, int e, const uint16_t *list, int cnt)
{
	const col_ent_t *cv = ent_cv(e);
	int16_t bb[6];
	float d[4];
	int i, k;

	for (k = 0; k < 3; k++)
	{
		bb[k] = (int16_t)floorf(cv->absmin[k]);
		bb[k + 3] = (int16_t)ceilf(cv->absmax[k]);
	}
	plane_d(L->gofs, d);
	if (box_culled(bb, d))
		return 0;
	for (i = 0; i < cnt; i++)
	{
		const int16_t *sb = L->sects[list[i]].bbox;

		for (k = 0; k < 3; k++)
			if (bb[k] > sb[k + 3] || bb[k + 3] < sb[k])
				break;
		if (k == 3)
			return 1;
	}
	return 0;
}

static int mover_opaque(int e)
{
	int m = ent_cv(e)->rendermode;

	return m == 0 || m == 4;	/* Normal, TransAlpha (alpha compare) */
}

/* One mover under its own matrix (opaque pass or the translucent pass). */
static void draw_mover(wld_layer_t *L, int e, int lit)
{
	const w64_sub_t *sb = &L->subs[mover_sub(L, e)];
	float org[3];
	ren_mat_t mt;
	uint8_t *ch;
	int k, n;

	for (k = 0; k < 3; k++)
		org[k] = L->gofs[k] + ent_cv(e)->origin[k];
	if (!(ch = wld_chunk_use(L, sb->chunk)))
	{
		st.missing++;
		return;
	}
	/* meshlets are culled by the linked box already: keep them all */
	{
		static const float none[4] = { -1e30f, -1e30f, -1e30f, -1e30f };

		n = add_meshlets(L, 0, sb->chunk, ch, none);
	}
	if (!n)
		return;
	st.movers++;
	mover_matrix(e, org, &mt);
	gpu_matrix_push(&mt);
	draw_list(L, n, org, lit);
	gpu_matrix_pop();
}

void ren_mover_draw(int e, int gpu_mode, const uint8_t prim[4])
{
	wld_layer_t *L;

	if (!ent_is_live(e) || !(L = wld_layer(ent_idc(e)->layer)) || mover_sub(L, e) < 0)
		return;
	gpu_fx_mode(gpu_mode, prim);
	last_bind = NULL;
	draw_mover(L, e, 0);
	last_bind = NULL;
}

/* ---- flashlight pool (dlights.md 3) ----------------------------------- */

#define FP_BATCH	64		/* vertices per gpu_draw */
#define FP_SCALE	64.0f		/* pool positions: 1/64 u relative to the light */
#define FP_POLY		12

static const ren_dlight_t *flash;
static int		fp_cur, fp_used, fp_iused;

/* Keep the part of poly (n points) with m.x >= d. */
static int fp_clip(float (*in)[3], int n, const float m[3], float d, float (*out)[3])
{
	int i, k, o = 0;

	for (i = 0; i < n; i++)
	{
		const float *a = in[i], *b = in[(i + 1) % n];
		float da = m[0] * a[0] + m[1] * a[1] + m[2] * a[2] - d;
		float db = m[0] * b[0] + m[1] * b[1] + m[2] * b[2] - d;

		if (da >= 0 && o < FP_POLY)
			memcpy(out[o++], a, sizeof(out[0]));
		if ((da >= 0) != (db >= 0) && o < FP_POLY)
		{
			float t = da / (da - db);

			for (k = 0; k < 3; k++)
				out[o][k] = a[k] + t * (b[k] - a[k]);
			o++;
		}
	}
	return o;
}

static float fp_dot(const float a[3], const float b[3])
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void fp_cross(const float a[3], const float b[3], float o[3])
{
	o[0] = a[1] * b[2] - a[2] * b[1];
	o[1] = a[2] * b[0] - a[0] * b[2];
	o[2] = a[0] * b[1] - a[1] * b[0];
}

/* Submit the batch [*v0, fp_used) pairs / [*i0, fp_iused) indices. The
 * next batch's indices start on the next 8-byte boundary (gpu.h
 * gpu_draw): the RSP DMA drops an index pointer's low 3 bits, and a
 * shifted strip both draws wrong triangles and, when the last index it
 * sees opens a strip, steps past the strip's end and runs on through DMEM
 * (ares: RSP CRASH in rspq_next_buffer, RDP busy, a frame after the
 * flashlight came on). Vertex pairs are 32 bytes: always aligned. */
static void fp_flush(int *v0, int *i0)
{
	int nv = (fp_used - *v0) * 2, ni = fp_iused - *i0;

	if (ni > 0)
	{
		gpu_writeback(&fp_v[fp_cur][*v0], (uint32_t)(fp_used - *v0) * sizeof(w64_vert_t));
		gpu_writeback(&fp_i[fp_cur][*i0], (uint32_t)ni * sizeof(int16_t));
		gpu_draw(&fp_v[fp_cur][*v0], nv, &fp_i[fp_cur][*i0], ni);
	}
	fp_iused = (fp_iused + FP_IDX_ALIGN - 1) & ~(FP_IDX_ALIGN - 1);
	*v0 = fp_used;
	*i0 = fp_iused;
}

/* The spot on one base triangle (p3 in drawn winding, layer-local; cooked
 * vertices vi of vp): 8 wedges of the falloff cone around the light's
 * foot, each clipped to the triangle; shade = base (barycentric) +
 * falloff x brightness before the light scale, st from the base's
 * gradients; strip winding = the base's. */
static void fp_tri(const wld_layer_t *L, const float lo[3], float p3[3][3],
	const w64_vert_t *vp, const int vi[3], int *v0, int *i0)
{
	float e1[3], e2[3], n[3], u[3], w[3], foot[3], len, pd, reach, area;
	float b = (float)flash->color[0] / 255.0f;
	uint32_t ls = L->hdr.lightscale_x100;
	unsigned stride = L->hdr.idx_stride ? L->hdr.idx_stride : 1;
	int k, j, e;

	for (k = 0; k < 3; k++)
	{
		e1[k] = p3[1][k] - p3[0][k];
		e2[k] = p3[2][k] - p3[0][k];
	}
	fp_cross(e1, e2, n);
	area = sqrtf(fp_dot(n, n));
	if (area < 1e-3f)
		return;
	for (k = 0; k < 3; k++)
		n[k] /= area;
	pd = fp_dot(n, lo) - fp_dot(n, p3[0]);
	reach = flash->radius - (pd < 0 ? -pd : pd);
	if (reach < 1 || reach < flash->minlight)
		return;
	for (k = 0; k < 3; k++)
		foot[k] = lo[k] - pd * n[k];
	/* fan axes from the plane alone, not from the triangle's own edge:
	 * coplanar neighbours cut the same wedges, so their pieces meet on
	 * the shared edge at the same points (a per-triangle fan left
	 * T-junctions there: a dotted crack along the diagonals on ares) */
	{
		float ax[3] = { 0, 0, 1 };

		if (n[2] > 0.9f || n[2] < -0.9f)
		{
			ax[0] = 1;
			ax[2] = 0;
		}
		fp_cross(ax, n, u);
	}
	len = sqrtf(fp_dot(u, u));
	for (k = 0; k < 3; k++)
		u[k] /= len;
	fp_cross(n, u, w);
	for (j = 0; j < 8; j++)
	{
		float poly[FP_POLY][3], tmp[FP_POLY][3];
		float c0 = cosf((float)j * 0.785398f), s0 = sinf((float)j * 0.785398f);
		float c1 = cosf((float)(j + 1) * 0.785398f), s1 = sinf((float)(j + 1) * 0.785398f);
		int np = 3, i, first, lo_i = 0, hi_i, s;

		for (k = 0; k < 3; k++)
		{
			poly[0][k] = foot[k];
			poly[1][k] = foot[k] + reach * (c0 * u[k] + s0 * w[k]);
			poly[2][k] = foot[k] + reach * (c1 * u[k] + s1 * w[k]);
		}
		for (e = 0; e < 3 && np >= 3; e++)
		{
			const float *q0 = p3[e], *q1 = p3[(e + 1) % 3];
			float ed[3], m[3];

			for (k = 0; k < 3; k++)
				ed[k] = q1[k] - q0[k];
			fp_cross(n, ed, m);
			if (fp_dot(m, p3[(e + 2) % 3]) < fp_dot(m, q0))
				for (k = 0; k < 3; k++)
					m[k] = -m[k];
			np = fp_clip(poly, np, m, fp_dot(m, q0), tmp);
			memcpy(poly, tmp, sizeof(poly[0]) * (size_t)np);
		}
		if (np < 3)
			continue;
		if ((fp_used - *v0) * 2 + np > FP_BATCH)
			fp_flush(v0, i0);
		if (fp_used + (np + 1) / 2 > FP_PAIRS || fp_iused + np > FP_IDX)
			return;		/* the frame's pool is full */
		first = (fp_used - *v0) * 2;
		for (i = 0; i < np; i++)
		{
			w64_vert_t *o = &fp_v[fp_cur][fp_used + (i >> 1)];
			int16_t *pos = (i & 1) ? o->posB : o->posA;
			int16_t *st2 = (i & 1) ? o->stB : o->stA;
			uint32_t *rgba = (i & 1) ? &o->rgbaB : &o->rgbaA;
			float r[3], x[3], wt[3], base = 0, q, add, sv[2] = { 0, 0 };
			uint32_t c8;
			int t;

			/* barycentric of the base triangle */
			for (k = 0; k < 3; k++)
				r[k] = poly[i][k] - p3[0][k];
			fp_cross(r, e2, x);
			wt[1] = fp_dot(x, n) / area;
			fp_cross(e1, r, x);
			wt[2] = fp_dot(x, n) / area;
			wt[0] = 1 - wt[1] - wt[2];
			for (t = 0; t < 3; t++)
			{
				const w64_vert_t *pr = &vp[vi[t] >> 1];
				const int16_t *bst = (vi[t] & 1) ? pr->stB : pr->stA;

				base += wt[t] * (float)(((vi[t] & 1) ? pr->rgbaB : pr->rgbaA) >> 24);
				sv[0] += wt[t] * bst[0];
				sv[1] += wt[t] * bst[1];
			}
			for (k = 0; k < 3; k++)
				r[k] = poly[i][k] - foot[k];
			q = sqrtf(fp_dot(r, r));
			add = reach - q > 0 ? (reach - q) * b : 0;
			c8 = (uint32_t)(base + add * (float)ls / 100.0f + 0.5f);
			if (c8 > 255)
				c8 = 255;
			*rgba = c8 << 24 | c8 << 16 | c8 << 8 | (vp[vi[0] >> 1].rgbaA & 0xFF);
			for (k = 0; k < 3; k++)
				pos[k] = (int16_t)lrintf((poly[i][k] - lo[k]) * FP_SCALE);
			st2[0] = (int16_t)lrintf(sv[0]);
			st2[1] = (int16_t)lrintf(sv[1]);
			if (!(i & 1))
				o->normA = o->normB = vp[vi[0] >> 1].normA;
		}
		/* the convex piece as one strip: 0, 1, n-1, 2, n-2, ... */
		hi_i = np - 1;
		for (s = 0; s < np; s++)
		{
			int pick = (s == 0 || (s & 1)) ? lo_i++ : hi_i--;
			uint16_t ix = (uint16_t)(L->hdr.idx_base + (unsigned)(first + pick) * stride);

			fp_i[fp_cur][fp_iused++] = (int16_t)(s == 0 ? ix | W64_IDX_RESTART : ix);
		}
		fp_used += (np + 1) / 2;
		st.pool_verts += (uint32_t)np;
		st.pool_tris += (uint32_t)(np - 2);
	}
}

/* The flashlight's second pass over the layer's drawn list (dlights.md 3):
 * decal depth, one matrix (the light, 1/64 u). */
static void flash_pool(wld_layer_t *L, int n, const ren_dlight_t *fl)
{
	unsigned stride = L->hdr.idx_stride ? L->hdr.idx_stride : 1, base = L->hdr.idx_base;
	float lo[3], r = fl->radius;
	ren_mat_t mt;
	int i, j, k, v0 = fp_used, i0 = fp_iused;

	for (k = 0; k < 3; k++)
		lo[k] = fl->origin[k] - L->gofs[k];
	memset(&mt, 0, sizeof(mt));
	for (k = 0; k < 3; k++)
	{
		mt.m[k][k] = 1.0f / FP_SCALE;
		mt.m[3][k] = fl->origin[k] - eye_g[k];
	}
	mt.m[3][3] = 1.0f;
	gpu_matrix_push(&mt);
	gpu_decal_depth(1);
	for (i = 0; i < n; i++)
	{
		const vis_t *e = &vis[i];
		const w64_meshlet_t *m = e->m;
		const w64_vert_t *vp = (const w64_vert_t *)(e->chunk + W64_MESHLET_VOFS(m));
		const int16_t *idx = (const int16_t *)(e->chunk + W64_MESHLET_IOFS(m));
		const w64_texp_t *tp = L->ch[L->dir->texp_first + e->page].data;
		int ni = W64_MESHLET_NI(m), nv = W64_MESHLET_NV(m), s0 = 0;

		for (k = 0; k < 3; k++)
			if (lo[k] + r < m->bbox[k] || lo[k] - r > m->bbox[k + 3])
				break;
		if (k < 3)
			continue;
		if ((const void *)tp != last_bind)
		{
			fp_flush(&v0, &i0);
			gpu_bind_page(tp, L->page_state + (uint32_t)e->page * WLD_PAGE_STATE);
			last_bind = tp;
			st.binds++;
		}
		for (j = 0; j < ni; j++)
		{
			int vi[3], t;
			float p3[3][3];

			if ((uint16_t)idx[j] & W64_IDX_RESTART)
				s0 = j;
			if (j - s0 < 2)
				continue;
			for (t = 0; t < 3; t++)
			{
				vi[t] = (int)(((uint16_t)idx[j - 2 + t] & 0x3FFF) - base) / (int)stride;
				if (vi[t] < 0 || vi[t] >= nv)
					break;
			}
			if (t < 3)
				continue;
			if ((j - s0) & 1)
			{
				t = vi[0];
				vi[0] = vi[1];
				vi[1] = t;
			}
			for (t = 0; t < 3; t++)
			{
				const int16_t *s = (vi[t] & 1) ? vp[vi[t] >> 1].posB : vp[vi[t] >> 1].posA;

				for (k = 0; k < 3; k++)
					p3[t][k] = s[k];
			}
			fp_tri(L, lo, p3, vp, vi, &v0, &i0);
		}
	}
	fp_flush(&v0, &i0);
	gpu_decal_depth(0);
	gpu_matrix_pop();
}

static void draw_layer(wld_layer_t *L)
{
	const uint16_t *list;
	float d[4], eye_l[3];
	int i, k, n = 0, cnt, moved = 0;

	for (i = 0; i < 3; i++)
		eye_l[i] = eye_g[i] - L->gofs[i];
	plane_d(L->gofs, d);
	st.leaf = (uint32_t)wld_point_leaf(L, eye_l);
	for (i = 0; i < L->dir->ntex; i++)
		tex_page0[i] = L->texs[ren_tex_anim(L->texs, L->dir->ntex, i, 0, r10)].page;

	list = wld_leaf_sectors(L, (int)st.leaf, &cnt);
	for (i = 0; i < cnt; i++)
	{
		const w64_sector_t *s = &L->sects[list[i]];
		uint8_t *ch;

		if (box_culled(s->bbox, d))
			continue;
		ch = wld_chunk_use(L, s->rend_chunk);
		if (!ch)
		{
			st.missing++;
			continue;
		}
		st.sectors++;
		n = add_meshlets(L, n, s->rend_chunk, ch, d);
	}
	collect_movers(L);
	/* movers at rest at the map origin join the world list */
	if (nmov)
	{
		for (i = 0; i < nmov; i++)
		{
			int e = mov_ent[i];
			const w64_sub_t *sb;
			uint8_t *ch;

			if (!mover_opaque(e) || (ent_rnd(e)->effects & EF_NODRAW)
				|| rotated(ent_cv(e)->origin) || rotated(ent_cv(e)->angles)
				|| !ent_visible(L, e, list, cnt))
				continue;
			sb = &L->subs[mover_sub(L, e)];
			if (!(ch = wld_chunk_use(L, sb->chunk)))
			{
				st.missing++;
				continue;
			}
			st.movers++;
			n = add_meshlets(L, n, sb->chunk, ch, d);
		}
	}
	else
		for (i = 0; i < L->nmovers; i++)
		{
			const wld_mover_t *mv = &L->movers[i];
			const w64_sub_t *sb = &L->subs[mv->sub];
			uint8_t *ch;

			if (!at_origin(mv) || !mover_visible(L, mv, list, cnt) || box_culled(sb->bbox, d))
				continue;
			ch = wld_chunk_use(L, sb->chunk);
			if (!ch)
			{
				st.missing++;
				continue;
			}
			st.movers++;
			n = add_meshlets(L, n, sb->chunk, ch, d);
		}
	push_translation(L->gofs);
	draw_list(L, n, L->gofs, 1);
	gpu_matrix_pop();
	if (flash)
		flash_pool(L, n, flash);

	if (nmov)
	{
		/* moved / rotated opaque movers under their matrix; translucent
		 * ones to the translucent pass (blend 0: not drawn, still solid) */
		for (i = 0; i < nmov; i++)
		{
			int e = mov_ent[i];
			float c[3], d2 = 0;

			if ((ent_rnd(e)->effects & EF_NODRAW) || !ent_visible(L, e, list, cnt))
				continue;
			if (!mover_opaque(e))
			{
				for (k = 0; k < 3; k++)
				{
					c[k] = L->gofs[k] + 0.5f * (ent_cv(e)->absmin[k] + ent_cv(e)->absmax[k]);
					d2 += (c[k] - eye_g[k]) * (c[k] - eye_g[k]);
				}
				ren_trans_brush(e, d2);
				continue;
			}
			if (!rotated(ent_cv(e)->origin) && !rotated(ent_cv(e)->angles))
				continue;	/* in the world list */
			if (moved >= HL64_REN_MOVERS_MAX)
			{
				st.dropped++;
				continue;
			}
			moved++;
			draw_mover(L, e, 1);
		}
		return;
	}

	/* M1 placements with an origin: one translated list each */
	for (i = 0; i < L->nmovers; i++)
	{
		const wld_mover_t *mv = &L->movers[i];
		const w64_sub_t *sb = &L->subs[mv->sub];
		float org[3], dm[4];
		uint8_t *ch;

		if (at_origin(mv) || !mover_visible(L, mv, list, cnt))
			continue;
		for (k = 0; k < 3; k++)
			org[k] = L->gofs[k] + mv->origin[k];
		plane_d(org, dm);
		if (box_culled(sb->bbox, dm))
			continue;
		if (moved >= HL64_REN_MOVERS_MAX)
		{
			st.dropped++;
			continue;
		}
		ch = wld_chunk_use(L, sb->chunk);
		if (!ch)
		{
			st.missing++;
			continue;
		}
		st.movers++;
		n = add_meshlets(L, 0, sb->chunk, ch, dm);
		if (!n)
			continue;
		moved++;
		push_translation(org);
		draw_list(L, n, org, 1);
		gpu_matrix_pop();
	}
}

/* ---- OPEN seams (cwg-format.md "OPEN seams") ------------------------------------- */

#define OSEAM_SCALE	(1.0f + 1.0f / 256.0f)	/* about the eye: A's copy wins the depth test */

/* Meshlet box (B-local) meets the carry box (B-local). */
static int touches(const int16_t *bb, const float lo[3], const float hi[3])
{
	int k;

	for (k = 0; k < 3; k++)
		if (bb[k] > hi[k] || bb[k + 3] < lo[k])
			return 0;
	return 1;
}

/* B's OPEN draw set (the leading ndraw sector-pool entries: B sectors,
 * 0x8000 | n B MOVR models), its meshlets touching the carry box, drawn
 * at B's origin - T and scaled about the eye by OSEAM_SCALE. */
static void draw_open_seam(wld_layer_t *B, const seam_info_t *s)
{
	float org[3], d[4];
	ren_mat_t mt;
	int i, j, k, n = 0, wait = 0;
	uint32_t before;

	for (k = 0; k < 3; k++)
		org[k] = B->gofs[k] + oseam.off[k];
	plane_d(org, d);
	for (i = 0; i < B->dir->ntex; i++)
		tex_page0[i] = B->texs[ren_tex_anim(B->texs, B->dir->ntex, i, 0, r10)].page;
	for (j = 0; j < s->ndraw && j < s->npool; j++)
	{
		uint16_t e = s->pool[j];
		const w64_rend_t *r;
		const w64_meshlet_t *m;
		uint8_t *ch;
		int c, page;

		if (e & 0x8000)
		{
			if ((e & 0x7FFF) >= B->dir->nsub || B->subs[e & 0x7FFF].kind != W64_SUB_MOVR)
				continue;
			c = B->subs[e & 0x7FFF].chunk;
		}
		else if (e < B->dir->nsect)
			c = B->sects[e].rend_chunk;
		else
			continue;
		if (!(ch = wld_chunk_use(B, c)))
		{
			wait = 1;
			continue;
		}
		r = (const w64_rend_t *)ch;
		m = (const w64_meshlet_t *)(ch + 24);
		for (k = 0; k < r->nmeshlets; k++, m++)
		{
			if (!touches(m->bbox, oseam.lo, oseam.hi))
				continue;
			page = tex_page0[m->tex];
			if (page == W64_NONE)
				continue;
			/* every page of the set, in view or not (turning must not
			 * hide B again) */
			if (!wld_chunk_use(B, B->dir->texp_first + page))
			{
				wait = 1;
				continue;
			}
			if (box_culled(m->bbox, d))
				continue;
			if (n >= HL64_REN_VIS_MAX)
			{
				st.dropped++;
				continue;
			}
			vis[n].m = m;
			vis[n].chunk = ch;
			vis[n].chunk_idx = (uint16_t)c;
			vis[n].page = (uint16_t)page;
			n++;
		}
	}
	/* B shows through whole: until every chunk of the draw set landed
	 * (requested above) nothing of it draws, so nothing pops in */
	if (wait)
		return;
	if (!n)
		return;
	/* camera-relative: scaling about the eye scales the whole matrix */
	memset(&mt, 0, sizeof(mt));
	for (k = 0; k < 3; k++)
	{
		mt.m[k][k] = OSEAM_SCALE;
		mt.m[3][k] = (org[k] - eye_g[k]) * OSEAM_SCALE;
	}
	mt.m[3][3] = 1.0f;
	gpu_matrix_push(&mt);
	before = st.meshlets;
	draw_list(B, n, org, 1);
	gpu_matrix_pop();
	st.oseam_meshlets += st.meshlets - before;
	st.meshlets = before;
	if (settings.r_openseam == 2)
		log_printf("ren: open seam %s->%s meshlets=%d\n", wld_map_name(s->from),
			wld_map_name(s->to), n);
}

/* The OPEN seams of home layer A whose approach box holds the player:
 * their B layer (ready) draws its draw set. */
static void open_seams(const wld_layer_t *A)
{
	seam_graph_t g;
	seam_info_t s;
	const float *po;
	int i, k;

	oseam.slot = -1;
	if (!settings.r_openseam || seam_graph(&g) || !phy_player_active()
		|| phy_player_map() != A->map)
		return;
	po = phy_player()->origin;
	for (i = 0; i < g.nseams; i++)
	{
		wld_layer_t *B;

		if (seam_info(i, &s) || s.from != A->map || !(s.flags & SEAM_F_OPEN) || !s.ndraw)
			continue;
		for (k = 0; k < 3; k++)
			if (po[k] < s.ap_mins[k] || po[k] > s.ap_maxs[k])
				break;
		if (k < 3)
			continue;
		B = wld_layer_of_map(s.to);
		/* B shows through once its seam set is resident (nothing of it
		 * pops in through the seam) */
		if (!B || B->ready != 1 || !wld_layer(B->slot) || !wld_pin_resident(B))
			continue;
		/* B content at g_B - T: B's landmark on A's; the carry box
		 * (A-local) in B's frame is + delta */
		oseam.slot = B->slot;
		for (k = 0; k < 3; k++)
		{
			oseam.off[k] = -s.T[k];
			oseam.lo[k] = s.carry_mins[k] + s.delta[k];
			oseam.hi[k] = s.carry_maxs[k] + s.delta[k];
		}
		draw_open_seam(B, &s);
		/* A's translucent movers (trans pass) use A's pages again */
		for (k = 0; k < A->dir->ntex; k++)
			tex_page0[k] = A->texs[ren_tex_anim(A->texs, A->dir->ntex, k, 0, r10)].page;
		return;		/* one approached seam at a time */
	}
}

int ren_open_layer(int slot, float off[3], float lo[3], float hi[3])
{
	if (slot < 0 || slot != oseam.slot)
		return 0;
	memcpy(off, oseam.off, sizeof(oseam.off));
	memcpy(lo, oseam.lo, sizeof(oseam.lo));
	memcpy(hi, oseam.hi, sizeof(oseam.hi));
	return 1;
}

void ren_other_layer_ent(int drawn)
{
	if (drawn)
		st.oseam_ents++;
	else
		st.other_ents++;
}

void ren_frame(double time)
{
	uint64_t t0 = sys_time_us();
	gpu_view_t v;
	float fx, fy, sx, cx, sy, cy;
	int i, k;
	wld_layer_t *L;

	memset(&st, 0, sizeof(st));
	r10 = (uint32_t)(time * 10.0);
	st.r10 = r10;
	memcpy(eye_g, ren_cam.org, sizeof(eye_g));
	ren_angle_axes(ren_cam.ang, v.fwd, v.right, v.up);
	v.fov_y = ren_fov_y(settings.fov);
	v.znear = ZNEAR;
	v.zfar = ZFAR;
	/* inward side planes: fwd x sin(half fov) +- axis x cos(half fov) */
	fx = settings.fov * (3.14159265358979f / 360.0f);
	fy = v.fov_y * (3.14159265358979f / 360.0f);
	sx = sinf(fx);
	cx = cosf(fx);
	sy = sinf(fy);
	cy = cosf(fy);
	for (k = 0; k < 3; k++)
	{
		fr_n[0][k] = v.fwd[k] * sx + v.right[k] * cx;
		fr_n[1][k] = v.fwd[k] * sx - v.right[k] * cx;
		fr_n[2][k] = v.fwd[k] * sy + v.up[k] * cy;
		fr_n[3][k] = v.fwd[k] * sy - v.up[k] * cy;
	}
	ndl = 0;
	flash = NULL;
	for (i = 0; i < HL64_DLIGHTS; i++)
		if (ren_dlight_live(i) && ren_dlight(i)->world)
		{
			if (ren_dlight(i)->flash)
				flash = ren_dlight(i);	/* the pool pass, not vertices */
			else
				dl[ndl++] = ren_dlight(i);
		}
	dl_cur ^= 1;
	dl_used = 0;
	fp_cur ^= 1;
	fp_used = fp_iused = 0;
	last_bind = NULL;

	gpu_begin(&v);
	fx_draw_begin();
	oseam.slot = -1;
	for (i = 0; i < HL64_WORLD_LAYERS; i++)
		if ((L = wld_layer(i)) && L->map == ren_cam.map)
		{
			draw_layer(L);
			open_seams(L);
		}
	st.world_us = (uint32_t)(sys_time_us() - t0);
	/* frame step 11 (architecture.md 3.2): studio, particles,
	 * translucent, decals, view model; the HUD follows */
	studio_draw(time);
	fx_draw_particles();
	fx_draw_trans();
	fx_draw_decals();
	studio_draw_view();
	gpu_end();
	last = st;
	wld_stream_missing(st.missing);
}

const float (*ren_frustum(void))[3]
{
	return (const float (*)[3])fr_n;
}

void ren_stats(ren_stats_t *s)
{
	*s = last;
}
