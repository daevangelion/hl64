/* SPDX-License-Identifier: MIT */
/*
 * decal.c -- decals (docs/specs/render-fx.md 4, docs/architecture.md 9.7),
 * written from the spec. The old tree's r64_decal.c was not used.
 *
 * Sheet: gsfx/decals.i4 (tools/cook/stage.py stage_decals), loaded at boot
 * into the decal region; its entry i is HLSDK decals.h index i (dlls/
 * world.cpp gDecals order, as the game names decals), no names at run time.
 *
 * Placement: every cooked triangle of the hit model (the REND chunks of
 * the world sectors around the point, or the mover's MOVR chunk in its
 * local frame) whose plane passes within 4 u of the point and which does
 * not face away from the hit normal receives the decal: a rectangle of the
 * decal's world size centred on the point projected onto that plane,
 * aligned with the triangle's texture axes (from its texel coordinates),
 * clipped to the triangle (Sutherland-Hodgman, <= 8 vertices kept per
 * piece, <= HL64_FX_DECAL_VERTS per decal). A decal keeps its pieces in
 * its model's frame (map-local or mover-local, quarter units), so mover
 * decals move with the mover and a decal whose sector is evicted is kept
 * and drawn again when the sector returns. Pool HL64_FX_DECALS, oldest
 * replaced. Placements are queued (HL64_FX_DECAL_QUEUE): processed under
 * 3 ms a frame; one whose sector is not resident waits (4 retried per
 * frame) and is dropped after 2 s.
 */
#include <math.h>
#include <string.h>

#include "hl64/ent.h"
#include "hl64/grid.h"
#include "hl64/limits.h"
#include "engine/core/log.h"
#include "engine/core/sys.h"
#include "engine/mem/carve.h"
#include "engine/render/fx_int.h"
#include "engine/render/ren.h"
#include "engine/world/wld.h"

#define SHEET_MAGIC	0x44493402u	/* 'DI4' 2: HLSDK decals.h order */
#define SHEET_ENTRY	28
#define SHEET_MAX	64		/* decals in a sheet */
#define REACH		4.0f		/* plane distance that receives the decal */
#define PIECE_MAX	8		/* clip vertices kept per piece */
#define PIECES		8		/* pieces per decal */
#define BUDGET_US	3000
#define RETRIES		4
#define QUEUE_LIFE	2.0
#define MAX_CHUNKS	16		/* chunks one placement may touch */

typedef struct {
	uint8_t		tw, th, ww, wh;		/* texels, world units */
	uint8_t		tint[3], pad;
	const uint8_t	*i4;
} dtex_t;

typedef struct {
	int16_t		pos[3];		/* model frame, quarter units */
	int16_t		st[2];		/* texels, s10.5 */
} dvert_t;				/* 10 */

typedef struct {
	uint32_t	seq;		/* placement order (oldest replaced) */
	ent_h		owner;		/* mover; 0 = world */
	uint16_t	chunk;		/* REND / MOVR chunk the pieces lie on */
	uint8_t		live, layer, tex, nverts;
	uint8_t		npieces, pad;
	uint8_t		piece[PIECES];	/* vertices per piece (fans) */
	uint16_t	map;
} fxdecal_t;

typedef struct {
	float		p[3], n[3];	/* global */
	double		t0;
	ent_h		owner;
	int16_t		decal;
	uint8_t		live, tries;
} pend_t;

static dtex_t		tex[SHEET_MAX];
static int		ntex;
static fxdecal_t	pool[HL64_FX_DECALS];
static dvert_t		*verts;			/* HL64_FX_DECALS x HL64_FX_DECAL_VERTS */
static pend_t		queue[HL64_FX_DECAL_QUEUE];
static uint32_t		seq;
static decal_stats_t	st;

/* ---- sheet ------------------------------------------------------------------------ */

static uint32_t be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* Validate the sheet (hostile input); 0 ok. */
static int load_sheet(uint8_t *b, uint32_t size)
{
	int n, i;

	if (size < 8 || be32(b) != SHEET_MAGIC || (b[6] << 8 | b[7]) != SHEET_ENTRY)
		return -1;
	n = b[4] << 8 | b[5];
	if (n < 1 || n > SHEET_MAX || 8u + (uint32_t)n * SHEET_ENTRY > size)
		return -2;
	for (i = 0; i < n; i++)
	{
		const uint8_t *e = b + 8 + SHEET_ENTRY * i;
		uint32_t ofs = be32(e + 24), bytes = (uint32_t)e[16] * e[17] / 2;

		if (!e[16] || !e[17] || e[16] > 64 || e[17] > 64 || (e[16] & 15) || ofs & 7
			|| ofs > size || bytes > size - ofs || !e[18] || !e[19])
			return -3;
		tex[i].tw = e[16];
		tex[i].th = e[17];
		tex[i].ww = e[18];
		tex[i].wh = e[19];
		memcpy(tex[i].tint, e + 20, 3);
		tex[i].i4 = b + ofs;
	}
	ntex = n;
	return 0;
}

void decal_init(void)
{
	uint8_t *region = mem_region(MEM_DECAL_TEXELS);
	uint32_t cap = mem_region_bytes(MEM_DECAL_TEXELS);
	uint32_t vbytes = HL64_FX_DECALS * HL64_FX_DECAL_VERTS * (uint32_t)sizeof(dvert_t);
	uint32_t size = 0, rom;
	int bad;

	memset(pool, 0, sizeof(pool));
	memset(queue, 0, sizeof(queue));
	memset(&st, 0, sizeof(st));
	ntex = 0;
	seq = 0;
	if (vbytes > cap)
		sys_error("fx: decal vertices %lu B > decal region", (unsigned long)vbytes);
	verts = (dvert_t *)(region + cap - vbytes);
	rom = sys_cart_addr("gsfx/decals.i4", &size);
	if (!rom)
		return;
	if (size > cap - vbytes)
		sys_error("gsfx/decals.i4: %lu B > decal region left %lu B", (unsigned long)size,
			(unsigned long)(cap - vbytes));
	if (!sys_cart_read(region, cap - vbytes, rom, size, 0, size))
		sys_error("gsfx/decals.i4: read");
	if ((bad = load_sheet(region, size)))
		sys_error("gsfx/decals.i4: bad sheet (rule %d)", bad);
	log_printf("mem fx: decals sheet=%lu verts=%lu region=%lu decals=%d\n", (unsigned long)size,
		(unsigned long)vbytes, (unsigned long)cap, ntex);
}

/* ---- geometry ------------------------------------------------------------------------- */

typedef struct {
	float		p[3];
} v3_t;

static float dot(const float a[3], const float b[3])
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void cross(const float a[3], const float b[3], float o[3])
{
	o[0] = a[1] * b[2] - a[2] * b[1];
	o[1] = a[2] * b[0] - a[0] * b[2];
	o[2] = a[0] * b[1] - a[1] * b[0];
}

static float normalize(float v[3])
{
	float l = sqrtf(dot(v, v));

	if (l > 1e-8f)
	{
		v[0] /= l;
		v[1] /= l;
		v[2] /= l;
	}
	return l;
}

/* Keep the part of poly (n verts) on the inner side of plane (m, d): m.x >= d. */
static int clip(v3_t *in, int n, const float m[3], float d, v3_t *out)
{
	int i, k, o = 0;

	for (i = 0; i < n; i++)
	{
		const float *a = in[i].p, *b = in[(i + 1) % n].p;
		float da = dot(m, a) - d, db = dot(m, b) - d;

		if (da >= 0 && o < PIECE_MAX)
			out[o++] = in[i];
		if ((da >= 0) != (db >= 0) && o < PIECE_MAX)
		{
			float t = da / (da - db);

			for (k = 0; k < 3; k++)
				out[o].p[k] = a[k] + t * (b[k] - a[k]);
			o++;
		}
	}
	return o;
}

/* Placement context: one decal texture, the point and normal in the
 * model's frame, the pieces gathered so far (per chunk). */
typedef struct {
	const dtex_t	*t;
	int		tex;
	float		q[3], n[3];	/* model frame */
	int		facing;		/* hit normal known */
	int		sign;		/* winding -> face normal sign */
	float		reach2;		/* bbox reach */
	/* output of the current chunk */
	dvert_t		v[HL64_FX_DECAL_VERTS];
	uint8_t		piece[PIECES];
	int		nv, np, dropped_verts;
} place_t;

/* A chunk's bytes once its read has landed (a LOADING chunk already owns
 * its block, but the DMA has not filled it); re-validated at every use. */
static uint8_t *landed(wld_layer_t *L, int c)
{
	const wld_chunk_t *ch = &L->ch[c];

	return ch->state == WLD_CH_RESIDENT || ch->state == WLD_CH_PINNED ? ch->data : NULL;
}

static void vpos(const w64_vert_t *vp, int i, float o[3])
{
	const w64_vert_t *pr = &vp[i >> 1];
	const int16_t *s = (i & 1) ? pr->posB : pr->posA;

	o[0] = s[0];
	o[1] = s[1];
	o[2] = s[2];
}

static void vst(const w64_vert_t *vp, int i, float o[2])
{
	const w64_vert_t *pr = &vp[i >> 1];
	const int16_t *s = (i & 1) ? pr->stB : pr->stA;

	o[0] = (float)s[0] / 32.0f;
	o[1] = (float)s[1] / 32.0f;
}

static int16_t q16(float v)
{
	v *= GPU_FX_POS_SCALE;
	return (int16_t)(v > 32767.0f ? 32767 : v < -32767.0f ? -32767 : lrintf(v));
}

/* One triangle: receive the decal if it qualifies. */
static void tri(place_t *P, const w64_vert_t *vp, int i0, int i1, int i2)
{
	float a[3], b[3], c[3], e1[3], e2[3], n[3], d, u[3], v[3], ctr[3];
	float sa[2], sb[2], sc[2];
	v3_t poly[PIECE_MAX], tmp[PIECE_MAX];
	int k, np, e;

	vpos(vp, i0, a);
	vpos(vp, i1, b);
	vpos(vp, i2, c);
	for (k = 0; k < 3; k++)
	{
		e1[k] = b[k] - a[k];
		e2[k] = c[k] - a[k];
	}
	cross(e1, e2, n);
	if (normalize(n) < 1e-4f)
		return;
	for (k = 0; k < 3; k++)
		n[k] *= (float)P->sign;
	if (P->facing && dot(n, P->n) < -0.01f)
		return;			/* faces away from the hit */
	d = dot(n, P->q) - dot(n, a);
	if (d > REACH || d < -REACH)
		return;
	/* texture axes from the texel coordinates (s right, t down) */
	vst(vp, i0, sa);
	vst(vp, i1, sb);
	vst(vp, i2, sc);
	{
		float ds1 = sb[0] - sa[0], ds2 = sc[0] - sa[0], dt1 = sb[1] - sa[1], dt2 = sc[1] - sa[1];
		float det = ds1 * dt2 - ds2 * dt1;

		if (det > 1e-6f || det < -1e-6f)
			for (k = 0; k < 3; k++)
			{
				u[k] = (e1[k] * dt2 - e2[k] * dt1) / det;
				v[k] = (e2[k] * ds1 - e1[k] * ds2) / det;
			}
		else
		{
			/* no usable texel gradient: world up projected, else x */
			float up[3] = { 0, 0, 1 };

			if (n[2] > 0.9f || n[2] < -0.9f)
			{
				up[0] = 1;
				up[2] = 0;
			}
			cross(up, n, u);
			cross(n, u, v);
		}
	}
	/* in-plane, orthonormal */
	d = dot(u, n);
	for (k = 0; k < 3; k++)
		u[k] -= d * n[k];
	if (normalize(u) < 1e-6f)
		return;
	cross(n, u, ctr);
	if (dot(ctr, v) < 0)
		for (k = 0; k < 3; k++)
			ctr[k] = -ctr[k];
	memcpy(v, ctr, sizeof(v));
	d = dot(n, P->q) - dot(n, a);
	for (k = 0; k < 3; k++)
		ctr[k] = P->q[k] - d * n[k];
	/* the rectangle, then clipped by the triangle's three edges */
	for (e = 0; e < 4; e++)
	{
		float sx = (e == 1 || e == 2) ? 0.5f : -0.5f, sy = e >= 2 ? 0.5f : -0.5f;

		for (k = 0; k < 3; k++)
			poly[e].p[k] = ctr[k] + u[k] * sx * P->t->ww + v[k] * sy * P->t->wh;
	}
	np = 4;
	{
		const float *pts[3] = { a, b, c };

		for (e = 0; e < 3 && np >= 3; e++)
		{
			const float *p0 = pts[e], *p1 = pts[(e + 1) % 3];
			float ed[3], m[3];

			for (k = 0; k < 3; k++)
				ed[k] = p1[k] - p0[k];
			cross(n, ed, m);
			/* inward: towards the third vertex */
			if (dot(m, pts[(e + 2) % 3]) < dot(m, p0))
				for (k = 0; k < 3; k++)
					m[k] = -m[k];
			np = clip(poly, np, m, dot(m, p0), tmp);
			memcpy(poly, tmp, sizeof(poly[0]) * (size_t)np);
		}
	}
	if (np < 3)
		return;
	if (P->nv + np > HL64_FX_DECAL_VERTS || P->np >= PIECES)
	{
		P->dropped_verts += np;
		return;
	}
	for (e = 0; e < np; e++)
	{
		dvert_t *o = &P->v[P->nv + e];
		float r[3];

		for (k = 0; k < 3; k++)
		{
			o->pos[k] = q16(poly[e].p[k]);
			r[k] = poly[e].p[k] - ctr[k];
		}
		o->st[0] = (int16_t)lrintf((dot(r, u) / P->t->ww + 0.5f) * P->t->tw * 32.0f);
		o->st[1] = (int16_t)lrintf((dot(r, v) / P->t->wh + 0.5f) * P->t->th * 32.0f);
	}
	P->piece[P->np++] = (uint8_t)np;
	P->nv += np;
}

/* Walk the meshlets of one resident chunk near the point. */
/* The meshlet's vertices and strip lie inside the chunk's bytes. */
static int meshlet_ok(const w64_meshlet_t *m, uint32_t size)
{
	uint32_t vofs = W64_MESHLET_VOFS(m), iofs = W64_MESHLET_IOFS(m);
	uint32_t nv = (uint32_t)W64_MESHLET_NV(m), ni = (uint32_t)W64_MESHLET_NI(m);

	return vofs <= size && (nv + 1) / 2 * sizeof(w64_vert_t) <= size - vofs && iofs <= size
		&& ni * 2u <= size - iofs && !(vofs & 3) && !(iofs & 1);
}

static int chunk_ok(const uint8_t *ch, uint32_t size)
{
	const w64_rend_t *r = (const w64_rend_t *)ch;

	return size >= sizeof(w64_rend_t)
		&& r->nmeshlets <= (size - sizeof(w64_rend_t)) / sizeof(w64_meshlet_t);
}

static void walk_chunk(place_t *P, const wld_layer_t *L, const uint8_t *ch, uint32_t size)
{
	const w64_rend_t *r = (const w64_rend_t *)ch;
	const w64_meshlet_t *m = (const w64_meshlet_t *)(ch + sizeof(w64_rend_t));
	unsigned stride = L->hdr.idx_stride ? L->hdr.idx_stride : 1, base = L->hdr.idx_base;
	float reach = sqrtf(P->reach2);
	int i, k;

	if (!chunk_ok(ch, size))
		return;
	for (i = 0; i < r->nmeshlets; i++, m++)
	{
		const w64_vert_t *vp = (const w64_vert_t *)(ch + W64_MESHLET_VOFS(m));
		const int16_t *idx = (const int16_t *)(ch + W64_MESHLET_IOFS(m));
		int ni = W64_MESHLET_NI(m), nv = W64_MESHLET_NV(m), s0 = 0, j;

		if (!meshlet_ok(m, size))
			continue;

		for (k = 0; k < 3; k++)
			if (P->q[k] + reach < m->bbox[k] || P->q[k] - reach > m->bbox[k + 3])
				break;
		if (k < 3)
			continue;
		/* strips: a RESTART-flagged index starts one */
		for (j = 0; j < ni; j++)
		{
			uint16_t e = (uint16_t)idx[j];
			int vi;

			if (e & W64_IDX_RESTART)
				s0 = j;
			if (j - s0 < 2)
				continue;
			{
				int a = (int)(((uint16_t)idx[j - 2] & 0x3FFF) - base) / (int)stride;
				int b = (int)(((uint16_t)idx[j - 1] & 0x3FFF) - base) / (int)stride;

				vi = (int)((e & 0x3FFF) - base) / (int)stride;
				if (a < 0 || b < 0 || vi < 0 || a >= nv || b >= nv || vi >= nv)
					continue;
				if ((j - s0) & 1)
					tri(P, vp, b, a, vi);
				else
					tri(P, vp, a, b, vi);
			}
		}
	}
}

static int alloc_slot(void)
{
	int i, old = 0;

	for (i = 0; i < HL64_FX_DECALS; i++)
	{
		if (!pool[i].live)
			return i;
		if (pool[i].seq < pool[old].seq)
			old = i;
	}
	st.replaced++;
	return old;
}

/* Store the chunk's pieces as one decal. */
static int commit(place_t *P, int layer, int map, int chunk, ent_h owner)
{
	fxdecal_t *d;
	int i;

	if (!P->nv)
		return 0;
	i = alloc_slot();
	d = &pool[i];
	memset(d, 0, sizeof(*d));
	d->seq = ++seq;
	d->owner = owner;
	d->chunk = (uint16_t)chunk;
	d->live = 1;
	d->layer = (uint8_t)layer;
	d->map = (uint16_t)map;
	d->tex = (uint8_t)P->tex;
	d->nverts = (uint8_t)P->nv;
	d->npieces = (uint8_t)P->np;
	memcpy(d->piece, P->piece, sizeof(d->piece));
	memcpy(&verts[i * HL64_FX_DECAL_VERTS], P->v, sizeof(dvert_t) * (size_t)P->nv);
	if (P->nv > st.verts_max)
		st.verts_max = (uint16_t)P->nv;
	st.pieces = (uint16_t)(st.pieces + P->np);
	P->nv = P->np = 0;
	return 1;
}

static void reset_out(place_t *P)
{
	P->nv = P->np = 0;
}

/* Winding sign: the hit face (coplanar with the point, normal along the
 * hit normal) decides whether strip winding gives outward normals. */
static int winding_sign(const place_t *P, const wld_layer_t *L, const uint8_t *ch, uint32_t size)
{
	const w64_rend_t *r = (const w64_rend_t *)ch;
	const w64_meshlet_t *m = (const w64_meshlet_t *)(ch + sizeof(w64_rend_t));
	unsigned stride = L->hdr.idx_stride ? L->hdr.idx_stride : 1, base = L->hdr.idx_base;
	int i, j, k, votes = 0;

	if (!chunk_ok(ch, size))
		return 1;
	for (i = 0; i < r->nmeshlets; i++, m++)
	{
		const w64_vert_t *vp = (const w64_vert_t *)(ch + W64_MESHLET_VOFS(m));
		const int16_t *idx = (const int16_t *)(ch + W64_MESHLET_IOFS(m));
		int ni = W64_MESHLET_NI(m), nv = W64_MESHLET_NV(m), s0 = 0;

		if (!meshlet_ok(m, size))
			continue;

		for (k = 0; k < 3; k++)
			if (P->q[k] + 1 < m->bbox[k] || P->q[k] - 1 > m->bbox[k + 3])
				break;
		if (k < 3)
			continue;
		for (j = 0; j < ni; j++)
		{
			int a, b, c;
			float pa[3], pb[3], pc[3], e1[3], e2[3], n[3], d;

			if ((uint16_t)idx[j] & W64_IDX_RESTART)
				s0 = j;
			if (j - s0 < 2)
				continue;
			a = (int)(((uint16_t)idx[j - 2] & 0x3FFF) - base) / (int)stride;
			b = (int)(((uint16_t)idx[j - 1] & 0x3FFF) - base) / (int)stride;
			c = (int)(((uint16_t)idx[j] & 0x3FFF) - base) / (int)stride;
			if (a < 0 || b < 0 || c < 0 || a >= nv || b >= nv || c >= nv)
				continue;
			if ((j - s0) & 1)
			{
				int t = a;

				a = b;
				b = t;
			}
			vpos(vp, a, pa);
			vpos(vp, b, pb);
			vpos(vp, c, pc);
			for (k = 0; k < 3; k++)
			{
				e1[k] = pb[k] - pa[k];
				e2[k] = pc[k] - pa[k];
			}
			cross(e1, e2, n);
			if (normalize(n) < 1e-4f)
				continue;
			d = dot(n, P->q) - dot(n, pa);
			if (d > 1 || d < -1)
				continue;
			d = dot(n, P->n);
			if (d > 0.99f)
				votes++;
			else if (d < -0.99f)
				votes--;
		}
	}
	return votes < 0 ? -1 : 1;
}

/* Place now. Returns pieces placed (>= 0), -1 when a needed chunk is not
 * resident (retry later). */
static int place(const float p_g[3], const float n_g[3], ent_h owner, int decal)
{
	place_t P;
	int li, k, placed = 0, missing = 0, sheet;

	if (decal < 0 || decal >= ntex || !tex[sheet = decal].tw)
	{
		st.nosheet++;
		return 0;
	}
	memset(&P, 0, sizeof(P));
	P.t = &tex[sheet];
	P.tex = sheet;
	P.facing = (n_g[0] != 0 || n_g[1] != 0 || n_g[2] != 0);
	{
		float r = 0.5f * sqrtf((float)P.t->ww * P.t->ww + (float)P.t->wh * P.t->wh) + REACH;

		P.reach2 = r * r;
	}
	if (owner)
	{
		/* a brush mover: its MOVR chunk in its local frame */
		int e = ent_of(owner), slot, sub;
		res_t m;
		wld_layer_t *L;
		const w64_sub_t *sb;
		float f[3], rt[3], up[3], d[3];
		const float *gm;

		if (e < 0)
			return 0;
		m = ent_rnd(e)->model;
		if (RES_KIND(m) != RES_K_SUBMODEL)
			return 0;
		slot = RES_SUB_SLOT(m);
		sub = RES_SUB_NUM(m);
		if (!(L = wld_layer(slot)) || sub >= L->dir->nsub)
			return 0;
		sb = &L->subs[sub];
		if (sb->kind != W64_SUB_MOVR)
			owner = 0;	/* folded into the sectors: a world decal */
		else
		{
			uint8_t *ch;

			if (!(ch = landed(L, sb->chunk)) || L->reencode)
				return -1;
			gm = eng_layer_origin(ent_idc(e)->layer);
			ren_angle_axes(ent_cv(e)->angles, f, rt, up);
			for (k = 0; k < 3; k++)
				d[k] = p_g[k] - (ent_cv(e)->origin[k] + gm[k]);
			P.q[0] = dot(d, f);
			P.q[1] = -dot(d, rt);
			P.q[2] = dot(d, up);
			P.n[0] = dot(n_g, f);
			P.n[1] = -dot(n_g, rt);
			P.n[2] = dot(n_g, up);
			P.sign = P.facing ? winding_sign(&P, L, ch, L->chunks[sb->chunk].size) : 1;
			walk_chunk(&P, L, ch, L->chunks[sb->chunk].size);
			placed = commit(&P, L->slot, L->map, sb->chunk, owner) ? P.np + 1 : 0;
			st.dropped += P.dropped_verts ? 1 : 0;
			return placed;
		}
	}
	/* the world: sectors of every open layer around the point */
	for (li = 0; li < HL64_WORLD_LAYERS; li++)
	{
		wld_layer_t *L = wld_layer(li);
		int s, sign = 0;
		uint16_t chunks[MAX_CHUNKS];
		int nch = 0;

		if (!L || L->reencode)
			continue;
		for (k = 0; k < 3; k++)
		{
			P.q[k] = p_g[k] - L->gofs[k];
			P.n[k] = n_g[k];
		}
		for (s = 0; s < L->dir->nsect; s++)
		{
			const w64_sector_t *sc = &L->sects[s];
			float r = sqrtf(P.reach2);
			int c;

			for (k = 0; k < 3; k++)
				if (P.q[k] + r < sc->bbox[k] || P.q[k] - r > sc->bbox[k + 3])
					break;
			if (k < 3)
				continue;
			c = sc->rend_chunk;
			for (k = 0; k < nch; k++)
				if (chunks[k] == c)
					break;
			if (k < nch || nch >= MAX_CHUNKS)
				continue;
			if (!landed(L, c))
			{
				wld_chunk_use(L, c);	/* ask for it; retry later */
				missing = 1;
				continue;
			}
			chunks[nch++] = (uint16_t)c;
		}
		if (missing)
			return -1;
		for (k = 0; k < nch && P.facing && !sign; k++)
		{
			const uint8_t *ch = landed(L, chunks[k]);

			if (ch)
				sign = winding_sign(&P, L, ch, L->chunks[chunks[k]].size);
		}
		P.sign = sign ? sign : 1;
		for (k = 0; k < nch; k++)
		{
			const uint8_t *ch = landed(L, chunks[k]);

			if (!ch)
				return -1;
			reset_out(&P);
			walk_chunk(&P, L, ch, L->chunks[chunks[k]].size);
			if (commit(&P, L->slot, L->map, chunks[k], 0))
				placed++;
		}
	}
	if (P.dropped_verts)
		st.dropped++;
	return placed;
}

/* ---- API ----------------------------------------------------------------------------------- */

void decal_queue(const float p_g[3], const float n[3], int e, int decal)
{
	pend_t *q = NULL;
	int i, old = -1;

	for (i = 0; i < HL64_FX_DECAL_QUEUE; i++)
	{
		if (!queue[i].live)
		{
			q = &queue[i];
			break;
		}
		if (old < 0 || queue[i].t0 < queue[old].t0)
			old = i;
	}
	if (!q)
	{
		q = &queue[old];	/* full: the oldest pending one is dropped */
		st.dropped++;
	}
	memcpy(q->p, p_g, sizeof(q->p));
	memcpy(q->n, n, sizeof(q->n));
	q->owner = e > 0 && ent_is_live(e) ? ent_handle_of(e) : 0;
	q->decal = (int16_t)decal;
	q->t0 = fx_s.now;
	q->tries = 0;
	q->live = 1;
}

int decal_place_now(const float p_g[3], const float n[3], int e, int decal)
{
	return place(p_g, n, e > 0 && ent_is_live(e) ? ent_handle_of(e) : 0, decal);
}

void fx_decal_trace(const col_trace_t *tr, int decal)
{
	float p[3];

	if (tr->frac >= 1.0f)
		return;
	fx_global(tr->end, p);
	decal_queue(p, tr->normal, tr->ent > 0 ? tr->ent : 0, decal);
}

void fx_decal_at(int e, const float org[3], int decal)
{
	static const float none[3] = { 0, 0, 0 };
	float p[3];

	fx_global(org, p);
	decal_queue(p, none, e > 0 ? e : 0, decal);
}

void decal_frame(void)
{
	uint64_t t0 = sys_time_us();
	int i, retries = 0, n = 0;

	for (i = 0; i < HL64_FX_DECAL_QUEUE; i++)
	{
		pend_t *q = &queue[i];
		int r;

		if (!q->live)
			continue;
		if (fx_s.now - q->t0 > QUEUE_LIFE)
		{
			q->live = 0;
			st.dropped++;
			continue;
		}
		if (sys_time_us() - t0 > BUDGET_US)
			break;		/* excess placements wait */
		if (q->tries && retries >= RETRIES)
			continue;
		if (q->tries)
			retries++;
		r = place(q->p, q->n, q->owner, q->decal);
		if (r < 0)
		{
			q->tries++;
			n++;
			continue;
		}
		q->live = 0;
		if (r > 0)
			st.placed++;
	}
	st.pending = (uint16_t)n;
	st.place_us += (uint32_t)(sys_time_us() - t0);
}

int decal_count(void)
{
	int i, n = 0;

	for (i = 0; i < HL64_FX_DECALS; i++)
		n += pool[i].live;
	return n;
}

void decal_stats(decal_stats_t *s, int reset)
{
	st.live = (uint16_t)decal_count();
	*s = st;
	if (reset)
	{
		uint16_t live = st.live, pending = st.pending;

		memset(&st, 0, sizeof(st));
		st.live = live;
		st.pending = pending;
	}
}

/* Frame of decal d now: origin (global) and axes; 0 when it is not drawable. */
static int frame_of(const fxdecal_t *d, float org[3], float ax[3][3])
{
	const wld_layer_t *L = wld_layer(d->layer);
	int k;

	if (!L || L->map != d->map || !landed((wld_layer_t *)L, d->chunk))
		return 0;
	memset(ax, 0, 9 * sizeof(float));
	if (d->owner)
	{
		int e = ent_of(d->owner);
		float f[3], r[3], u[3];
		const float *gm;

		if (e < 0)
			return -1;	/* the mover is gone: so is the decal */
		gm = eng_layer_origin(ent_idc(e)->layer);
		ren_angle_axes(ent_cv(e)->angles, f, r, u);
		for (k = 0; k < 3; k++)
		{
			org[k] = ent_cv(e)->origin[k] + gm[k];
			ax[0][k] = f[k];
			ax[1][k] = -r[k];
			ax[2][k] = u[k];
		}
		return 1;
	}
	for (k = 0; k < 3; k++)
	{
		org[k] = L->gofs[k];
		ax[k][k] = 1;
	}
	return 1;
}

static void world_of(const dvert_t *v, const float org[3], float ax[3][3], float out[3])
{
	float x = v->pos[0] / GPU_FX_POS_SCALE, y = v->pos[1] / GPU_FX_POS_SCALE;
	float z = v->pos[2] / GPU_FX_POS_SCALE;
	int k;

	for (k = 0; k < 3; k++)
		out[k] = org[k] + x * ax[0][k] + y * ax[1][k] + z * ax[2][k];
}

void decal_draw(void)
{
	int i, last = -1, set = 0;

	for (i = 0; i < HL64_FX_DECALS; i++)
	{
		fxdecal_t *d = &pool[i];
		const dvert_t *dv = &verts[i * HL64_FX_DECAL_VERTS];
		gpu_fx_vert_t v[HL64_FX_DECAL_VERTS];
		uint8_t idx[3 * HL64_FX_DECAL_VERTS];
		float org[3], ax[3][3];
		int k, j, ni = 0, b = 0, f;

		if (!d->live)
			continue;
		if ((f = frame_of(d, org, ax)) <= 0)
		{
			if (f < 0)
				d->live = 0;
			continue;
		}
		if (d->map != fx_s.map)
			continue;
		for (j = 0; j < d->nverts; j++)
		{
			float g[3];

			world_of(&dv[j], org, ax, g);
			if (!fx_cam_pos(g, v[j].pos))
				break;
			v[j].st[0] = dv[j].st[0];
			v[j].st[1] = dv[j].st[1];
			memcpy(v[j].rgba, tex[d->tex].tint, 3);
			v[j].rgba[3] = 255;
		}
		if (j < d->nverts)
			continue;
		/* each piece a fan */
		for (k = 0; k < d->npieces; k++)
		{
			for (j = 1; j + 1 < d->piece[k]; j++)
			{
				idx[ni++] = (uint8_t)b;
				idx[ni++] = (uint8_t)(b + j);
				idx[ni++] = (uint8_t)(b + j + 1);
			}
			b += d->piece[k];
		}
		if (!set)
		{
			gpu_fx_mode(GPU_FX_DECAL, NULL);
			set = 1;
		}
		if (d->tex != last)
		{
			gpu_fx_tex_i4(tex[d->tex].i4, tex[d->tex].tw, tex[d->tex].th);
			last = d->tex;
		}
		gpu_fx_tris(v, d->nverts, idx, ni / 3);
	}
}

int decal_info(int i, int *owner, int *nverts, float (*out)[3], int max)
{
	const fxdecal_t *d;
	float org[3], ax[3][3];
	int j;

	if (i < 0 || i >= HL64_FX_DECALS || !pool[i].live)
		return 0;
	d = &pool[i];
	*owner = d->owner ? ent_of(d->owner) : 0;
	*nverts = d->nverts;
	if (frame_of(d, org, ax) <= 0)
		return -1;
	for (j = 0; j < d->nverts && j < max; j++)
		world_of(&verts[i * HL64_FX_DECAL_VERTS + j], org, ax, out[j]);
	return d->npieces;
}
