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
 * live behind gpu.h. Not carried: the flashlight pool (the flashlight is
 * a vertex light, dlights.md 2), OPEN-seam cross-layer draw (M4).
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
#include "engine/core/sys.h"
#include "engine/mem/carve.h"
#include "engine/render/gpu_fx.h"
#include "engine/render/ren.h"
#include "engine/render/trans.h"
#include "engine/world/wld.h"

#define ZNEAR		4.0f
#define ZFAR		8192.0f
#define DL_PAIRS	512		/* dlit scratch per frame (vertex pairs) */

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

static w64_vert_t	*dl_buf[2];
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
	if (2u * DL_PAIRS * sizeof(w64_vert_t) > mem_region_bytes(MEM_DLIGHT))
		sys_error("render: dlight scratch > dlight region");
	dl_buf[0] = mem_region(MEM_DLIGHT);
	dl_buf[1] = dl_buf[0] + DL_PAIRS;
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

static void collect_movers(const wld_layer_t *L)
{
	int e;

	nmov = 0;
	for (e = 1; e < HL64_MAX_ENTITIES; e++)
		if (ent_is_live(e) && ent_idc(e)->layer == L->slot && mover_sub(L, e) >= 0)
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

	wld_want_open(L, (int)st.leaf);
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
	for (i = 0; i < HL64_DLIGHTS; i++)
		if (ren_dlight_live(i) && ren_dlight(i)->world)
			dl[ndl++] = ren_dlight(i);
	dl_cur ^= 1;
	dl_used = 0;
	last_bind = NULL;

	gpu_begin(&v);
	fx_draw_begin();
	for (i = 0; i < HL64_WORLD_LAYERS; i++)
		if ((L = wld_layer(i)) && L->map == ren_cam.map)
			draw_layer(L);
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
}

const float (*ren_frustum(void))[3]
{
	return (const float (*)[3])fr_n;
}

void ren_stats(ren_stats_t *s)
{
	*s = last;
}
