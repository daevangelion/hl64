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
 * live behind gpu.h. Night vision (architecture.md D12: the local
 * player's EF_DIMLIGHT) is a GPU mode of the frame (gpu_view_t.nvg): the
 * vertex light is not drawn then, so styled meshlets are not recoloured
 * and no dynamic light touches the world. OPEN
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
#include "engine/core/hlmath.h"
#include "engine/core/log.h"
#include "engine/core/sys.h"
#include "engine/phys/player.h"
#include "engine/mem/carve.h"
#include "engine/render/gpu_fx.h"
#include "engine/render/ren.h"
#include "engine/render/trans.h"
#include "engine/save/flq.h"
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
/* per texture: the page shown at this frame's r10 (world-surfaces.md 4)
 * on the world and frame-0 entities; set per layer by set_pages, read
 * per meshlet by add_meshlets */
static uint16_t		tex_page0[HL64_REN_TEX_MAX];
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
static int		dl_used, dl_cur;
static int		ndl;
static const ren_dlight_t *dl[HL64_DLIGHTS];
static float		norm5[32], norm6[64];
static int		nvg;		/* this frame: night vision (gpu_view_t.nvg) */
/* the layer / map / r10 tex_page0 holds (set_pages); tex_page1: the pages
 * for entities with frame != 0 (the alternate sequences) of layer
 * page1_of, built from tex_page0 by the first such entity drawn */
static const wld_layer_t *page0_of, *page1_of;
static int		page0_map = -1;
static uint32_t		page0_r10;
static uint16_t		tex_page1[HL64_REN_TEX_MAX];

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
	float sp, cp, sy, cy, sr, cr;

	hl_sincosf(ang[0] * k, &sp, &cp);
	hl_sincosf(ang[1] * k, &sy, &cy);
	hl_sincosf(ang[2] * k, &sr, &cr);

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

	return hl_atanf(hl_tanf(fov_x * k) * 0.75f) / k;
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

/* pages: tex_page0 (the world, frame-0 entities) or tex_page1 */
static int add_meshlets(wld_layer_t *L, int n, int c, uint8_t *ch, const float *d,
	const uint16_t *pages)
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
		page = pages[m->tex];
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
		/* night vision draws no vertex light: a style change waits for
		 * the next lit frame (the meshlet's epoch stays behind) */
		if (m->style != W64_NONE && !nvg)
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
 * column). Normal draws in the opaque pass (at rest at the map origin it
 * joins the world list); the other modes go to the translucent pass
 * (engine/render/trans.c), which calls ren_mover_draw back to front
 * (TransAlpha: lit, its page's alpha compare, Z write). A layer without
 * brush entities (no game, records not spawned: the M1 free-camera view)
 * draws the ENTS placements instead. */

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

/* A box (map-local) meets a visible sector and the frustum. */
static int box_visible(const wld_layer_t *L, const float lo[3], const float hi[3],
	const uint16_t *list, int cnt)
{
	int16_t bb[6];
	float d[4];
	int i, k;

	for (k = 0; k < 3; k++)
	{
		bb[k] = (int16_t)floorf(lo[k]);
		bb[k + 3] = (int16_t)ceilf(hi[k]);
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

/* The entity's linked box (map-local) meets a visible sector and the
 * frustum. */
static int ent_visible(const wld_layer_t *L, int e, const uint16_t *list, int cnt)
{
	return box_visible(L, ent_cv(e)->absmin, ent_cv(e)->absmax, list, cnt);
}

static int mover_opaque(int e)
{
	return ent_cv(e)->rendermode == 0;	/* Normal */
}

/* A brush entity's page table (L's tables set): its frame selects the
 * alternate textures (world-surfaces.md 4; HLSDK buttons and chargers
 * set frame 1). The alternate table is built once per set_pages, only
 * when an entity with frame != 0 draws. */
static const uint16_t *ent_pages(const wld_layer_t *L, int e)
{
	const w64_tex_t *x = L->texs;
	int i, n = L->dir->ntex;

	if (ent_rnd(e)->frame == 0)
		return tex_page0;
	if (page1_of != L)
	{
		for (i = 0; i < n; i++, x++)
			tex_page1[i] = (x->flags & W64_TEXF_ANIM) && x->alternate != W64_NONE
				? L->texs[ren_tex_anim(L->texs, n, i, 1, r10)].page : tex_page0[i];
		page1_of = L;
	}
	return tex_page1;
}

/* One mover under its own matrix (opaque pass or the translucent pass);
 * off: added to its global place (a seam ghost, seam_ghost), or NULL. */
static void draw_mover(wld_layer_t *L, int e, int lit, const float *off)
{
	const w64_sub_t *sb = &L->subs[mover_sub(L, e)];
	float org[3];
	ren_mat_t mt;
	uint8_t *ch;
	int k, n;

	for (k = 0; k < 3; k++)
		org[k] = L->gofs[k] + ent_cv(e)->origin[k] + (off ? off[k] : 0);
	if (!(ch = wld_chunk_use(L, sb->chunk)))
	{
		st.missing++;
		return;
	}
	/* meshlets are culled by the linked box already: keep them all */
	{
		static const float none[4] = { -1e30f, -1e30f, -1e30f, -1e30f };

		n = add_meshlets(L, 0, sb->chunk, ch, none, ent_pages(L, e));
	}
	if (!n)
		return;
	st.movers++;
	mover_matrix(e, org, &mt);
	gpu_matrix_push(&mt);
	draw_list(L, n, org, lit);
	gpu_matrix_pop();
}

/* Layer L's texture pages at r10 for frame-0 surfaces; kept while the
 * layer, its map and r10 are those of the last call. A still texture is
 * its own page; only '+' textures walk a sequence. */
static void set_pages(const wld_layer_t *L)
{
	const w64_tex_t *x = L->texs;
	int i, n = L->dir->ntex;

	if (L == page0_of && L->map == page0_map && r10 == page0_r10)
		return;
	page0_of = L;
	page0_map = L->map;
	page0_r10 = r10;
	page1_of = NULL;
	for (i = 0; i < n; i++, x++)
		tex_page0[i] = (x->flags & W64_TEXF_ANIM) ? L->texs[ren_tex_anim(L->texs, n, i, 0, r10)].page
			: x->page;
}

void ren_mover_draw(int e, int gpu_mode, const uint8_t prim[4])
{
	wld_layer_t *L, *H = NULL;
	float off[3];

	if (!ent_is_live(e) || !(L = wld_layer(ent_idc(e)->layer)) || mover_sub(L, e) < 0)
		return;
	/* a seam ghost (ghost_movers): B's pages, then the home layer's again */
	if (L->map != ren_cam.map)
	{
		if (!seam_ghost(e, ren_cam.map, off) || !(H = wld_layer_of_map(ren_cam.map)))
			return;
		set_pages(L);
	}
	gpu_fx_mode(gpu_mode, prim);
	/* additive: each page binds its ramp TLUT (render-fx.md 2a) */
	if (gpu_fx_rdp[gpu_mode].combiner == GPU_FX_CC_RAMP)
		gpu_page_ramp(prim);
	last_bind = NULL;
	draw_mover(L, e, gpu_mode == GPU_FX_BRUSH_SOLID, H ? off : NULL);
	gpu_page_ramp(NULL);
	last_bind = NULL;
	if (H)
		set_pages(H);
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
	set_pages(L);

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
		n = add_meshlets(L, n, s->rend_chunk, ch, d, tex_page0);
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
			n = add_meshlets(L, n, sb->chunk, ch, d, ent_pages(L, e));
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
			n = add_meshlets(L, n, sb->chunk, ch, d, tex_page0);
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
			draw_mover(L, e, 1, NULL);
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
		n = add_meshlets(L, 0, sb->chunk, ch, dm, tex_page0);
		if (!n)
			continue;
		moved++;
		push_translation(org);
		draw_list(L, n, org, 1);
		gpu_matrix_pop();
	}
}

/* ---- seam ghosts (seam_ghost; architecture.md 5.4 item 6) ------------------------- */

/* Home layer A's view of the approached seam's ghost brush entities (B's
 * movers drawn into B's carry box): under their own matrix at their
 * global place + off (-T), culled by A's visible sectors with their box
 * in A's frame; translucent ones to the translucent pass (ren_mover_draw). */
static void ghost_movers(wld_layer_t *A)
{
	int16_t ghost[SEAM_GHOSTS];
	const uint16_t *list = NULL;
	wld_layer_t *pages = NULL;
	float off[3];
	int i, k, cnt = 0, n = seam_ghosts(A->map, ghost, off);

	for (i = 0; i < n; i++)
	{
		int e = ghost[i];
		wld_layer_t *B;
		const col_ent_t *cv;
		float lo[3], hi[3], d2 = 0;

		/* live, drawn, in B's carry box (seam_ghosts) */
		if (!(B = wld_layer(ent_idc(e)->layer)) || mover_sub(B, e) < 0)
			continue;
		if (!list)
		{
			float eye_l[3];

			for (k = 0; k < 3; k++)
				eye_l[k] = eye_g[k] - A->gofs[k];
			list = wld_leaf_sectors(A, wld_point_leaf(A, eye_l), &cnt);
		}
		cv = ent_cv(e);
		for (k = 0; k < 3; k++)
		{
			float s = B->gofs[k] + off[k] - A->gofs[k];

			lo[k] = cv->absmin[k] + s;
			hi[k] = cv->absmax[k] + s;
		}
		if (!box_visible(A, lo, hi, list, cnt))
			continue;
		if (!mover_opaque(e))
		{
			for (k = 0; k < 3; k++)
			{
				float c = A->gofs[k] + 0.5f * (lo[k] + hi[k]) - eye_g[k];

				d2 += c * c;
			}
			ren_trans_brush(e, d2);
			continue;
		}
		if (pages != B)
		{
			set_pages(B);
			pages = B;
		}
		draw_mover(B, e, 1, off);
	}
	if (pages)
		set_pages(A);
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
	int j, k, n = 0, wait = 0;
	uint32_t before;

	for (k = 0; k < 3; k++)
		org[k] = B->gofs[k] + oseam.off[k];
	plane_d(org, d);
	set_pages(B);
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
		set_pages(A);
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
	sx = hl_sinf(fx);
	cx = hl_cosf(fx);
	sy = hl_sinf(fy);
	cy = hl_cosf(fy);
	for (k = 0; k < 3; k++)
	{
		fr_n[0][k] = v.fwd[k] * sx + v.right[k] * cx;
		fr_n[1][k] = v.fwd[k] * sx - v.right[k] * cx;
		fr_n[2][k] = v.fwd[k] * sy + v.up[k] * cy;
		fr_n[3][k] = v.fwd[k] * sy - v.up[k] * cy;
	}
	/* night vision: the local player's EF_DIMLIGHT (game/hl/player.c
	 * FlashlightTurnOn); the world then takes no dynamic light */
	nvg = ent_is_live(ENT_PLAYER) && (ent_rnd(ENT_PLAYER)->effects & EF_DIMLIGHT);
	v.nvg = (uint8_t)nvg;
	/* the brightness setting (menu.c): not under night vision */
	v.lift = nvg ? 0 : (uint8_t)SETTINGS_BRIGHT_LIFT(settings.brightness);
	st.nvg = (uint32_t)nvg;
	ndl = 0;
	for (i = 0; i < HL64_DLIGHTS && !nvg; i++)
		if (ren_dlight_live(i) && ren_dlight(i)->world)
			dl[ndl++] = ren_dlight(i);
	dl_cur ^= 1;
	dl_used = 0;
	last_bind = NULL;

	gpu_begin(&v);
	fx_draw_begin();
	oseam.slot = -1;
	for (i = 0; i < HL64_WORLD_LAYERS; i++)
		if ((L = wld_layer(i)) && L->map == ren_cam.map)
		{
			draw_layer(L);
			ghost_movers(L);
			open_seams(L);
			flq_tick();	/* a save's next flash page (flq.c) */
		}
	st.world_us = (uint32_t)(sys_time_us() - t0);
	/* frame step 11 (architecture.md 3.2): studio, particles,
	 * translucent, decals, view model; the HUD follows (a save's next
	 * flash page between them, flq.c) */
	studio_draw(time);
	flq_tick();
	fx_draw_particles();
	flq_tick();
	fx_draw_trans();
	flq_tick();
	fx_draw_decals();
	flq_tick();
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
