/* SPDX-License-Identifier: MIT */
/*
 * coll.c -- point contents, hull walks, traces, the move query, hitboxes,
 * surface materials and the collision window (include/hl64/coll.h).
 *
 * Written from docs/specs/trace-and-hulls.md and docs/formats/w64-format.md
 * ("Collision", "Surface materials"). A hull is a cooked COLL tree (or a
 * six-node box tree built on the spot for box entities, 4.3); the walks
 * keep an explicit stack of split nodes, so their depth is that of the
 * deepest tree (146 on hldemo3), never the call stack's.
 *
 * Segment walk (spec 3): a node whose plane separates the sub-segment is
 * split at the stand-off fraction (t1 -+ 1/32) / (t1 - t2); the near part is
 * walked first, then, if the far child classifies the split point as
 * non-solid, the far part; otherwise the plane is the impact, backed off in
 * tenths of the sub-segment while the point is still solid. Leaves update
 * the solid / open / water flags. Nothing allocates; the split-node stack
 * and the candidate list are static (traces do not nest).
 */
#include <math.h>
#include <string.h>

#include "hl64/coll.h"
#include "hl64/ent.h"
#include "hl64/limits.h"
#include "engine/core/dbg.h"
#include "engine/core/hlmath.h"
#include "engine/core/log.h"
#include "engine/core/sys.h"
#include "engine/save/flq.h"
#include "engine/save/sv.h"
#include "engine/save/svmod.h"
#include "engine/world/wld.h"

#define COL_EPS		(1.0f / 32.0f)	/* trace stand-off (DIST_EPSILON) */
#define COL_STACK	192		/* split nodes: deepest cooked tree 146 */
#define COL_WALK_MAX	1024		/* point walk steps: a deeper path is malformed */
#define COL_CBRUSH_MAX	64		/* linked contents brushes */
#define COL_WINDOW	128.0f		/* collision window margin (5.3) */

const float col_hull_mins[COL_HULLS][3] = {
	{ 0, 0, 0 }, { -16, -16, -36 }, { -32, -32, -32 }, { -16, -16, -18 }
};
const float col_hull_maxs[COL_HULLS][3] = {
	{ 0, 0, 0 }, { 16, 16, 36 }, { 32, 32, 32 }, { 16, 16, 18 }
};

static const char mat_chars[] = "CMDVGTSWPYFN";	/* W64_MAT_CHARS */
#define MAT_MIXED	15

/* A hull's current COLL chunk as the walks read it: the node, plane and
 * extern arrays and the axial ranges of its header, derived once per chunk
 * change instead of per node. */
typedef struct {
	const w64_cnode_t	*nodes;
	const w64_cplane_t	*planes;
	const uint32_t		*ext;		/* extern child gids */
	uint32_t		base;		/* the chunk's gid base: ci << W64_GID_LOCAL_BITS */
	uint32_t		nnodes, nx, ny, nz;
} view_t;

/* A hull to walk: a layer's COLL tree, or the box tree. */
typedef struct {
	wld_layer_t	*L;		/* NULL: box tree */
	int32_t		root;		/* gid, or a leaf child */
	uint8_t		hull0;		/* leaves are BSP leaf indices (hull 0) */
	/* the current chunk: index (COL_NO_CHUNK: none yet), data and view */
	uint32_t	ci;
	const uint8_t	*cp;
	view_t		v;
	/* impact node (materials) and its crossing point, model frame */
	const uint8_t	*hit_cp;
	uint32_t	hit_ci, hit_li;
	float		hit_p[3];
} src_t;

#define COL_NO_CHUNK	0xFFFFFFFFu	/* no gid's chunk: the first node reads its own */

typedef struct {
	int32_t		far;		/* far child: gid or leaf child */
	const uint8_t	*cp;		/* the split node's chunk */
	uint32_t	ci;
	uint16_t	li;		/* the split node */
	uint8_t		side;		/* 1: the sub-segment starts behind the plane */
	uint8_t		done;		/* its far part is being (or was) walked */
	float		frac;		/* stand-off fraction of the sub-segment */
	float		p1f, p2f;
	float		p1[3], p2[3];
} split_t;

static split_t		stack[COL_STACK];
static const col_ent_t	*cand[HL64_MAX_ENTITIES];
static const col_ent_t	*cbrush[COL_CBRUSH_MAX];
static int		ncbrush;
static col_hitbox_fn	hitbox_fn;
static col_stats_t	st;
static uint8_t		warned_nan, warned_cbrush;

/* ---- hull data ------------------------------------------------------------- */

static __attribute__((noreturn)) void bad_model(uint16_t model)
{
	sys_error("coll: model %u.%u is not an open layer's", COL_MODEL_SLOT(model),
		COL_MODEL_SUB(model));
}

#define HDR(cp)		((const w64_coll_t *)(cp))
#define NODES(cp)	((const w64_cnode_t *)((cp) + 16 + 16 * HDR(cp)->nplanes))
#define PLANES(cp)	((const w64_cplane_t *)((cp) + 16))

/* s->v from s->cp / s->ci */
static void chunk_view(src_t *s)
{
	const w64_coll_t *h = HDR(s->cp);

	s->v.nodes = NODES(s->cp);
	s->v.planes = PLANES(s->cp);
	s->v.ext = (const uint32_t *)(s->v.nodes + h->nnodes);
	s->v.base = s->ci << W64_GID_LOCAL_BITS;
	s->v.nnodes = h->nnodes;
	s->v.nx = h->nx;
	s->v.ny = h->ny;
	s->v.nz = h->nz;
}

/* COLL chunk ci of s's layer becomes the current chunk: resident, or read
 * now. Out of line: the walks' per-node path only compares the index. */
static __attribute__((noinline)) void chunk_switch(src_t *s, uint32_t ci)
{
	wld_layer_t *L = s->L;
	const uint8_t *cp;

	if (ci >= L->dir->ncoll)
		sys_error("coll: %s: node chunk %lu >= %u", wld_map_name(L->map), (unsigned long)ci,
			L->dir->ncoll);
	cp = wld_chunk_use(L, L->dir->coll_first + (int)ci);
	if (!cp)
	{
		uint64_t t0 = sys_time_us();

		cp = wld_chunk_sync(L, L->dir->coll_first + (int)ci);
		st.sync_loads++;
		st.sync_us += (uint32_t)(sys_time_us() - t0);
	}
	s->ci = ci;
	s->cp = cp;
	chunk_view(s);
}

/* Child c of a node of view v -> gid or leaf child. */
static inline int32_t child_of(const view_t *v, int c)
{
	if (c < 0)
		return c;
	if ((uint32_t)c < v->nnodes)
		return (int32_t)(v->base | (uint32_t)c);
	return (int32_t)v->ext[c - (int)v->nnodes];
}

/* Signed distance of p from a non-axial plane, in double. */
static inline float plane_eval(const w64_cplane_t *pl, const float p[3])
{
	return (float)((double)pl->normal[0] * p[0] + (double)pl->normal[1] * p[1]
		+ (double)pl->normal[2] * p[2] - pl->dist);
}

/* Plane of node li in chunk cp: axis 0-2 (unit normal) or 3 (pl). */
static float plane_dist(const uint8_t *cp, uint32_t li, const float p[3])
{
	const w64_coll_t *h = HDR(cp);
	const w64_cnode_t *nd = NODES(cp) + li;

	if (li < h->nx)
		return p[0] - nd->u.dist;
	if (li < h->ny)
		return p[1] - nd->u.dist;
	if (li < h->nz)
		return p[2] - nd->u.dist;
	return plane_eval(PLANES(cp) + nd->u.plane, p);
}

static void plane_get(const uint8_t *cp, uint32_t li, float n[3], float *d)
{
	const w64_coll_t *h = HDR(cp);
	const w64_cnode_t *nd = NODES(cp) + li;

	if (li < h->nz)
	{
		n[0] = n[1] = n[2] = 0;
		n[li < h->nx ? 0 : li < h->ny ? 1 : 2] = 1;
		*d = nd->u.dist;
	}
	else
	{
		const w64_cplane_t *pl = PLANES(cp) + nd->u.plane;

		n[0] = pl->normal[0];
		n[1] = pl->normal[1];
		n[2] = pl->normal[2];
		*d = pl->dist;
	}
}

static int leaf_contents(const src_t *s, int32_t ref)
{
	uint32_t leaf;

	if (!s->hull0)
		return ref;
	leaf = (uint32_t)W64_LEAF_OF(ref);
	if (leaf >= s->L->dir->nleafs_all)
		sys_error("coll: %s: leaf %lu >= %u", wld_map_name(s->L->map), (unsigned long)leaf,
			s->L->dir->nleafs_all);
	return s->L->leafc[leaf];
}

/* The leaf (contents) of p below node ref. */
static int walk_point(src_t *s, int32_t ref, const float p[3])
{
	view_t v = s->v;
	uint32_t cur = s->ci;
	int steps;

	for (steps = 0; ref >= 0; steps++)
	{
		uint32_t li = W64_GID_LOCAL(ref);
		const w64_cnode_t *nd;
		float d;

		if (W64_GID_COLL(ref) != cur)
		{
			chunk_switch(s, W64_GID_COLL(ref));
			v = s->v;
			cur = s->ci;
		}
		if (li >= v.nnodes)
			sys_error("coll: node %ld past its chunk", (long)ref);
		if (steps == COL_WALK_MAX)
			sys_error("coll: hull walk deeper than %d", COL_WALK_MAX);
		nd = v.nodes + li;
		if (li < v.nz)
			d = p[li < v.nx ? 0 : li < v.ny ? 1 : 2] - nd->u.dist;
		else
			d = plane_eval(v.planes + nd->u.plane, p);
		ref = child_of(&v, nd->child[d >= 0 ? 0 : 1]);
	}
	st.nodes += (uint32_t)steps;
	return leaf_contents(s, ref);
}

/* Submodel sub of layer L, hull hull (both valid). */
static void src_layer(src_t *s, wld_layer_t *L, unsigned sub, int hull)
{
	s->L = L;
	s->root = L->models[sub].head[hull];
	s->hull0 = hull == 0;
	s->ci = COL_NO_CHUNK;
	s->cp = NULL;
	s->v.nodes = NULL;
	s->v.planes = NULL;
	s->v.ext = NULL;
	s->v.base = s->v.nnodes = s->v.nx = s->v.ny = s->v.nz = 0;
	s->hit_cp = NULL;
}

static int src_model(src_t *s, uint16_t model, int hull)
{
	unsigned slot = COL_MODEL_SLOT(model), sub = COL_MODEL_SUB(model);
	wld_layer_t *L = wld_layer((int)slot);

	if (!L || sub >= L->dir->nsub || hull < 0 || hull >= COL_HULLS)
		return 0;
	src_layer(s, L, sub, hull);
	return 1;
}

/* Box tree (4.3): planes x = hi, x = lo, y = hi, y = lo, z = hi, z = lo;
 * outside any of them EMPTY, inside all SOLID. */
static uint8_t box_tree[16 + 6 * 8] __attribute__((aligned(8)));

static void src_box(src_t *s, const float lo[3], const float hi[3])
{
	w64_coll_t *h = (w64_coll_t *)box_tree;
	w64_cnode_t *nd = (w64_cnode_t *)(box_tree + 16);
	int k;

	h->nnodes = 6;
	h->nx = 2;
	h->ny = 4;
	h->nz = 6;
	h->nplanes = 0;
	h->nextern = 0;
	for (k = 0; k < 3; k++)
	{
		nd[2 * k].u.dist = hi[k];
		nd[2 * k].child[0] = COL_EMPTY;
		nd[2 * k].child[1] = (int16_t)(2 * k + 1);
		nd[2 * k + 1].u.dist = lo[k];
		nd[2 * k + 1].child[0] = k < 2 ? (int16_t)(2 * k + 2) : COL_SOLID;
		nd[2 * k + 1].child[1] = COL_EMPTY;
	}
	s->L = NULL;
	s->root = 0;
	s->hull0 = 0;
	s->ci = 0;
	s->cp = box_tree;
	chunk_view(s);
	s->hit_cp = NULL;
}

/* ---- segment walk (spec 3) -------------------------------------------------- */

static void lerp(float out[3], const float a[3], const float b[3], float f)
{
	out[0] = a[0] + f * (b[0] - a[0]);
	out[1] = a[1] + f * (b[1] - a[1]);
	out[2] = a[2] + f * (b[2] - a[2]);
}

/* Element copies: aligned single loads / stores (memcpy of a float[3]
 * compiles to unaligned pairs and byte moves here). */
static inline void copy3(float d[3], const float s[3])
{
	d[0] = s[0];
	d[1] = s[1];
	d[2] = s[2];
}

/* fr's far child is SOLID at the split point m (fraction mf): the plane
 * is the impact, backed off while the point is still solid. */
static __attribute__((noinline)) void trace_impact(src_t *s, const split_t *fr,
	const float split[3], float mf, col_trace_t *tr)
{
	float m[3], frac = fr->frac, t1, t2;

	copy3(m, split);
	s->cp = fr->cp;			/* the frame's chunk stays resident (used this frame) */
	s->ci = fr->ci;
	chunk_view(s);
	plane_get(fr->cp, fr->li, tr->normal, &tr->dist);
	t1 = plane_dist(fr->cp, fr->li, fr->p1);
	t2 = plane_dist(fr->cp, fr->li, fr->p2);
	if (fr->side)
	{
		tr->normal[0] = -tr->normal[0];
		tr->normal[1] = -tr->normal[1];
		tr->normal[2] = -tr->normal[2];
		tr->dist = -tr->dist;
	}
	s->hit_cp = fr->cp;
	s->hit_ci = fr->ci;
	s->hit_li = fr->li;
	lerp(s->hit_p, fr->p1, fr->p2, t1 / (t1 - t2));
	/* acute corners: back off while the point is still solid */
	while (walk_point(s, s->root, m) == COL_SOLID)
	{
		frac -= 0.1f;
		if (frac < 0)
			break;		/* keep the last point; never past the start */
		mf = fr->p1f + (fr->p2f - fr->p1f) * frac;
		lerp(m, fr->p1, fr->p2, frac);
	}
	tr->frac = mf;
	copy3(tr->end, m);
}

/* Walks p1 -> p2 (model frame) from s->root. Flags accumulate in tr;
 * returns 1 with tr->frac / end / normal / dist (model frame) on an
 * impact. tr->allsolid must start at 1.
 *
 * The far part of a split starts with the contents of the split point m
 * in the far child (spec 3). That is the first leaf the far part's own
 * walk reaches: at every node the walk of [m, p2] takes m's side (both
 * ends on one side: m's; split: the near child, m's), so the far part is
 * walked at once and its first leaf decides -- SOLID: the impact, the
 * frames above dropped; else the walk goes on -- instead of walking m's
 * path twice. A NaN distance of m (no side) falls back to the separate
 * point walk. */
static int walk_trace(src_t *s, const float p1[3], const float p2[3], col_trace_t *tr)
{
	float a[3], b[3], af = 0, bf = 1;
	int32_t ref = s->root;
	int steps = 0;
	view_t v = s->v;
	uint32_t cur = s->ci;
	split_t *top = stack, *fr, *first = NULL;	/* first: the frame whose far part is before its first leaf */

	copy3(a, p1);
	copy3(b, p2);
	for (;;)
	{
		int c;

		while (ref >= 0)
		{
			uint32_t li = W64_GID_LOCAL(ref);
			const w64_cnode_t *nd;
			float t1, t2, frac;
			int side;

			if (W64_GID_COLL(ref) != cur)
			{
				chunk_switch(s, W64_GID_COLL(ref));
				v = s->v;
				cur = s->ci;
			}
			if (li >= v.nnodes)
				sys_error("coll: node %ld past its chunk", (long)ref);
			steps++;
			nd = v.nodes + li;
			if (li < v.nz)
			{
				int k = li < v.nx ? 0 : li < v.ny ? 1 : 2;

				t1 = a[k] - nd->u.dist;
				t2 = b[k] - nd->u.dist;
			}
			else
			{
				const w64_cplane_t *pl = v.planes + nd->u.plane;

				t1 = plane_eval(pl, a);
				t2 = plane_eval(pl, b);
			}
			if ((t1 >= 0 && t2 >= 0) || (t1 < 0 && t2 < 0))
			{
				ref = child_of(&v, nd->child[t1 < 0]);	/* front: 0, back: 1 */
				continue;
			}
			if (first && t1 != t1)
			{
				/* m has no side: classify it apart, then walk its far part */
				fr = first;
				top = first + 1;
				first = NULL;
				if (walk_point(s, fr->far, a) == COL_SOLID)
					goto impact;
				ref = fr->far;
				copy3(b, fr->p2);
				bf = fr->p2f;
				v = s->v;
				cur = s->ci;
				continue;
			}
			side = t1 < 0;
			frac = side ? (t1 + COL_EPS) / (t1 - t2) : (t1 - COL_EPS) / (t1 - t2);
			if (frac < 0)
				frac = 0;
			if (frac > 1)
				frac = 1;
			if (top == stack + COL_STACK)
				sys_error("coll: hull walk deeper than %d splits", COL_STACK);
			fr = top++;
			fr->far = child_of(&v, nd->child[side ^ 1]);
			fr->cp = s->cp;
			fr->ci = cur;
			fr->li = (uint16_t)li;
			fr->side = (uint8_t)side;
			fr->done = 0;
			fr->frac = frac;
			fr->p1f = af;
			fr->p2f = bf;
			copy3(fr->p1, a);
			copy3(fr->p2, b);
			ref = child_of(&v, nd->child[side]);
			bf = af + (bf - af) * frac;
			lerp(b, fr->p1, fr->p2, frac);
		}
		c = leaf_contents(s, ref);
		if (first)
		{
			/* a far part's first leaf: m's contents in the far child */
			fr = first;
			first = NULL;
			if (c == COL_SOLID)
				goto impact;
		}
		if (c != COL_SOLID)
		{
			tr->allsolid = 0;
			if (c == COL_EMPTY)
				tr->inopen = 1;
			else
				tr->inwater = 1;
		}
		else
			tr->startsolid = 1;

		/* the near part ended without an impact: the far parts, innermost
		 * first (a frame stays until the frames above it are done) */
		do
		{
			if (top == stack)
			{
				st.nodes += (uint32_t)steps;
				return 0;
			}
			fr = --top;
		} while (fr->done);
		top++;
		fr->done = 1;
		first = fr;
		ref = fr->far;
		af = fr->p1f + (fr->p2f - fr->p1f) * fr->frac;
		lerp(a, fr->p1, fr->p2, fr->frac);
		copy3(b, fr->p2);
		bf = fr->p2f;
	}

impact:
	/* fr's far child is SOLID at the split point m = a (fraction af) */
	st.nodes += (uint32_t)steps;
	if (tr->allsolid)
		return 0;		/* never left the solid */
	trace_impact(s, fr, a, af, tr);
	return 1;
}

/* ---- frames ---------------------------------------------------------------- */

static void angle_vectors(const float ang[3], float f[3], float r[3], float u[3])
{
	const float k = 3.14159265358979323846f / 180.0f;
	float sp = hl_sinf(ang[0] * k), cp = hl_cosf(ang[0] * k);
	float sy = hl_sinf(ang[1] * k), cy = hl_cosf(ang[1] * k);
	float sr = hl_sinf(ang[2] * k), cr = hl_cosf(ang[2] * k);

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

static int rotated(const float *ang)
{
	return ang && (ang[0] != 0 || ang[1] != 0 || ang[2] != 0);
}

/* isfinite on the bits: an all-ones exponent is inf or NaN (no FPU
 * compare, no FPU hazard slots) */
static int finite3(const float v[3])
{
	union { float f; uint32_t u; } a, b, c;

	a.f = v[0];
	b.f = v[1];
	c.f = v[2];
	return (a.u & 0x7F800000u) != 0x7F800000u && (b.u & 0x7F800000u) != 0x7F800000u
		&& (c.u & 0x7F800000u) != 0x7F800000u;
}

/* Every field (no padding: the struct's pad is a field), without a
 * memset call. */
static inline void tr_init(col_trace_t *tr, const float end[3])
{
	tr->frac = 1;
	tr->end[0] = end[0];
	tr->end[1] = end[1];
	tr->end[2] = end[2];
	tr->normal[0] = tr->normal[1] = tr->normal[2] = 0;
	tr->dist = 0;
	tr->ent = -1;
	tr->allsolid = 1;
	tr->startsolid = tr->inopen = tr->inwater = 0;
	tr->hitgroup = 0;
	tr->pad = 0;
}

/* Rotated placement (spec 4.2): the segment into the model frame, the
 * impact back. Out of line: the world's and most entities' traces are
 * not rotated, and their path stays short. */
static __attribute__((noinline)) void trace_rotated(src_t *s, const float o[3],
	const float ang[3], const float start[3], const float end[3], col_trace_t *tr)
{
	float ls[3], le[3], f[3], r[3], u[3], a[3], b[3], n[3];
	int k;

	for (k = 0; k < 3; k++)
	{
		a[k] = start[k] - o[k];
		b[k] = end[k] - o[k];
	}
	angle_vectors(ang, f, r, u);
	ls[0] = a[0] * f[0] + a[1] * f[1] + a[2] * f[2];
	ls[1] = -(a[0] * r[0] + a[1] * r[1] + a[2] * r[2]);
	ls[2] = a[0] * u[0] + a[1] * u[1] + a[2] * u[2];
	le[0] = b[0] * f[0] + b[1] * f[1] + b[2] * f[2];
	le[1] = -(b[0] * r[0] + b[1] * r[1] + b[2] * r[2]);
	le[2] = b[0] * u[0] + b[1] * u[1] + b[2] * u[2];
	if (!walk_trace(s, ls, le, tr))
	{
		memcpy(tr->end, end, sizeof(tr->end));
		return;
	}
	memcpy(n, tr->normal, sizeof(n));
	for (k = 0; k < 3; k++)
	{
		tr->normal[k] = n[0] * f[k] - n[1] * r[k] + n[2] * u[k];
		tr->end[k] = start[k] + tr->frac * (end[k] - start[k]);
	}
}

static __attribute__((noinline, cold)) void trace_nan(col_trace_t *tr)
{
	if (!warned_nan)
		log_printf("coll: non-finite trace input: start solid\n");
	warned_nan = 1;
	tr->startsolid = 1;
}

/* Trace s placed at o (rotated by ang about o when non-zero): spec 4.2. */
static void trace_src(src_t *s, const float o[3], const float *ang, const float start[3],
	const float end[3], col_trace_t *tr)
{
	float ls[3], le[3];
	int k;

	tr_init(tr, end);
	st.traces++;
	if (!finite3(start) || !finite3(end) || !finite3(o))
	{
		trace_nan(tr);
		return;
	}
	if (rotated(ang))
	{
		trace_rotated(s, o, ang, start, end, tr);
		return;
	}
	for (k = 0; k < 3; k++)
	{
		ls[k] = start[k] - o[k];
		le[k] = end[k] - o[k];
	}
	if (!walk_trace(s, ls, le, tr))
	{
		copy3(tr->end, end);
		return;
	}
	for (k = 0; k < 3; k++)
		tr->end[k] += o[k];
}

/* ---- frames (architecture.md 8) ---------------------------------------------- */

/* Origin of layer L in the frame whose origin is f (eng_layer_origin of
 * eng_frame_layer()): g_M(L) - g_M(frame) (the frame's own layer: exactly 0). */
static inline void layer_in_frame(const wld_layer_t *L, const float f[3], float o[3])
{
	o[0] = L->gofs[0] - f[0];
	o[1] = L->gofs[1] - f[1];
	o[2] = L->gofs[2] - f[2];
}

static const float *frame_origin(void)
{
	return eng_layer_origin(eng_frame_layer());
}

/* A collision view in the entity layer's storage (else a caller's copy,
 * pmove physents: already in the caller's frame). */
static int stored(const col_ent_t *e)
{
	return e->index < HL64_MAX_ENTITIES && e == ent_cv(e->index);
}

/* ---- contents ------------------------------------------------------------------ */

static inline int layer_has(const wld_layer_t *L, const float o[3], const float p[3],
	const float lo[3], const float hi[3])
{
	const w64_model_t *m = &L->models[0];
	int k;

	for (k = 0; k < 3; k++)
		if (hi[k] + p[k] < m->mins[k] + o[k] || lo[k] + p[k] > m->maxs[k] + o[k])
			return 0;
	return 1;
}

static const float zero3[3] = { 0, 0, 0 };

int col_true_contents(const float p[3])
{
	const float *f;
	int slot, c = COL_SOLID;

	if (!finite3(p))
		return COL_SOLID;
	f = frame_origin();
	for (slot = 0; slot < HL64_WORLD_LAYERS && c == COL_SOLID; slot++)
	{
		wld_layer_t *L = wld_layer(slot);
		float lp[3], o[3];

		if (!L)
			continue;
		layer_in_frame(L, f, o);
		if (!layer_has(L, o, p, zero3, zero3))
			continue;
		lp[0] = p[0] - o[0];
		lp[1] = p[1] - o[1];
		lp[2] = p[2] - o[2];
		c = L->leafc[wld_point_leaf(L, lp)];
	}
	return c;
}

int col_contents(const float p[3])
{
	int i, c = col_true_contents(p);

	/* inside a contents brush its skin wins, except over world SOLID
	 * [probe:measured T-points] */
	if (c == COL_SOLID)
		return c;
	for (i = 0; i < ncbrush; i++)
	{
		const col_ent_t *e = cbrush[i];
		float d[3], o[3];
		int k;

		ent_frame_delta(e->index, d);
		for (k = 0; k < 3; k++)
			if (!(p[k] >= e->absmin[k] + d[k] && p[k] <= e->absmax[k] + d[k]))
				break;
		if (k < 3)
			continue;
		for (k = 0; k < 3; k++)
			o[k] = e->origin[k] + d[k];
		if (col_model_contents(e->model, o, e->angles, 0, p) != COL_EMPTY)
			return e->skin;
	}
	return c <= COL_CURRENT_0 && c >= COL_CURRENT_DOWN ? COL_WATER : c;
}

void col_contents_add(const col_ent_t *e)
{
	int i;

	for (i = 0; i < ncbrush; i++)
		if (cbrush[i] == e)
			return;
	if (ncbrush == COL_CBRUSH_MAX)
	{
		if (!warned_cbrush)
			log_printf("coll: more than %d contents brushes: entity %u ignored\n",
				COL_CBRUSH_MAX, e->index);
		warned_cbrush = 1;
		return;
	}
	cbrush[ncbrush++] = e;
}

void col_contents_remove(const col_ent_t *e)
{
	int i;

	for (i = 0; i < ncbrush; i++)
		if (cbrush[i] == e)
		{
			memmove(&cbrush[i], &cbrush[i + 1], (size_t)(ncbrush - i - 1) * sizeof(cbrush[0]));
			ncbrush--;
			return;
		}
}

int col_model_contents(uint16_t model, const float origin[3], const float angles[3], int hull,
	const float p[3])
{
	src_t s;
	float d[3], lp[3];

	if (!src_model(&s, model, hull))
		bad_model(model);
	if (!finite3(p))
		return COL_SOLID;
	d[0] = p[0] - origin[0];
	d[1] = p[1] - origin[1];
	d[2] = p[2] - origin[2];
	if (rotated(angles))
	{
		float f[3], r[3], u[3];

		angle_vectors(angles, f, r, u);
		lp[0] = d[0] * f[0] + d[1] * f[1] + d[2] * f[2];
		lp[1] = -(d[0] * r[0] + d[1] * r[1] + d[2] * r[2]);
		lp[2] = d[0] * u[0] + d[1] * u[1] + d[2] * u[2];
	}
	else
		memcpy(lp, d, sizeof(lp));
	return walk_point(&s, s.root, lp);
}

int col_hull_get(uint16_t model, int hull, const float origin[3], col_hull_t *h, float offset[3])
{
	src_t s;

	if (!src_model(&s, model, hull))
		return 0;
	h->model = model;
	h->hull = (uint8_t)hull;
	h->pad = 0;
	h->root = s.root;
	memcpy(offset, origin, 3 * sizeof(float));
	return 1;
}

int col_hull_contents(const col_hull_t *h, int32_t node, const float p[3])
{
	src_t s;

	if (!src_model(&s, h->model, h->hull))
		bad_model(h->model);
	if (!finite3(p))
		return COL_SOLID;
	return walk_point(&s, node, p);
}

/* GoldSrc spreads every brush model's bounds by 1 u: a map brush
 * entity's mins / maxs are the file's -/+ 1 on every axis [probe:measured
 * brush_ents of the fixtures: 348 / 348 unrotated brush entities on
 * t0a0a, t0a0b1, hldemo3]; HLSDK relies on it (func_door travel
 * size - 2) and a func_pushable collides with that box. */
int col_model_bounds(uint16_t model, float mins[3], float maxs[3])
{
	wld_layer_t *L = wld_layer(COL_MODEL_SLOT(model));
	unsigned sub = COL_MODEL_SUB(model);
	int k;

	if (!L || sub >= L->dir->nsub)
		return 0;
	for (k = 0; k < 3; k++)
	{
		mins[k] = L->models[sub].mins[k] - 1;
		maxs[k] = L->models[sub].maxs[k] + 1;
	}
	return 1;
}

/* ---- traces -------------------------------------------------------------------- */

int col_hull_for_size(const float mins[3], const float maxs[3])
{
	float sx = maxs[0] - mins[0], sz = maxs[2] - mins[2];

	if (sx <= 8)
		return 0;
	if (sx <= 36)
		return sz <= 36 ? 3 : 1;
	return 2;
}

void col_trace_model(uint16_t model, const float origin[3], const float angles[3], int hull,
	const float start[3], const float end[3], col_trace_t *tr)
{
	src_t s;

	if (!src_model(&s, model, hull))
		bad_model(model);
	trace_src(&s, origin, angles, start, end, tr);
}

/* Placement of hull k for a moving box (spec 2): base + hull.mins - mins
 * for the clip hulls; hull 0 traces the box's origin itself (a 0 x 18 box
 * stops with its origin 1/32 u off the surface) [probe:measured T-entities
 * hullsel]. */
static void hull_place(float o[3], const float base[3], int hull, const float mins[3])
{
	int k;

	for (k = 0; k < 3; k++)
		o[k] = hull ? base[k] + (col_hull_mins[hull][k] - mins[k]) : base[k];
}

/* Is a better than b (spec 4.1: nearest; equal: open over solid, lower slot)? */
static int nearer(const col_trace_t *a, const col_trace_t *b)
{
	if (a->frac != b->frac)
		return a->frac < b->frac;
	if (a->allsolid != b->allsolid)
		return b->allsolid;
	return a->startsolid < b->startsolid;
}

/* A world trace's inputs, for its per-layer step. */
typedef struct {
	const float	*start, *mins, *end;
	const float	*f;		/* the frame's origin */
	float		lo[3], hi[3];	/* the move's box */
	int		hull;
} wtrace_t;

/* Layer L's trace of w into d; 0 (d untouched): the move's box is
 * outside the layer. */
static __attribute__((noinline)) int trace_layer(wld_layer_t *L, const wtrace_t *w,
	col_trace_t *d)
{
	src_t s;
	float o[3], lo3[3];

	layer_in_frame(L, w->f, lo3);
	if (!layer_has(L, lo3, zero3, w->lo, w->hi))
		return 0;
	src_layer(&s, L, 0, w->hull);
	hull_place(o, lo3, w->hull, w->mins);
	trace_src(&s, o, NULL, w->start, w->end, d);
	if (d->frac < 1)
		d->dist += d->normal[0] * lo3[0] + d->normal[1] * lo3[1] + d->normal[2] * lo3[2];
	return 1;
}

void col_trace_world(const float start[3], const float mins[3], const float maxs[3],
	const float end[3], col_trace_t *tr)
{
	wtrace_t w;
	int slot, any = 0, k;

	w.start = start;
	w.mins = mins;
	w.end = end;
	w.f = frame_origin();
	w.hull = col_hull_for_size(mins, maxs);
	for (k = 0; k < 3; k++)
	{
		w.lo[k] = (start[k] < end[k] ? start[k] : end[k]) + mins[k];
		w.hi[k] = (start[k] < end[k] ? end[k] : start[k]) + maxs[k];
	}
	tr_init(tr, end);
	for (slot = 0; slot < HL64_WORLD_LAYERS; slot++)
	{
		wld_layer_t *L = wld_layer(slot);
		col_trace_t t;

		/* the first layer's trace is the result so far: straight into tr */
		if (!L || !trace_layer(L, &w, any ? &t : tr))
			continue;
		if (any && nearer(&t, tr))
			*tr = t;
		any = 1;
	}
	if (!any)
		tr->startsolid = 1;	/* outside every layer: solid (spec 1) */
	tr->ent = tr->frac < 1 || tr->startsolid ? 0 : -1;
}

/* e at origin org (the caller's frame). */
static void clip_at(const col_ent_t *e, const float org[3], const float start[3],
	const float mins[3], const float maxs[3], const float end[3], col_trace_t *tr)
{
	src_t s;
	int k;

	/* contents brushes (func_ladder, func_water: SOLID_NOT, skin < -1) are
	 * traced through their model too: PM_LadderMove aims at the ladder */
	if (e->solid == COL_SOLID_BSP || (e->model != COL_NONE && e->skin < -1))
	{
		int hull = col_hull_for_size(mins, maxs);
		float o[3];

		if (!src_model(&s, e->model, hull))
			bad_model(e->model);
		hull_place(o, org, hull, mins);
		trace_src(&s, o, e->angles, start, end, tr);
	}
	else
	{
		float lo[3], hi[3];

		for (k = 0; k < 3; k++)
		{
			lo[k] = e->mins[k] - maxs[k];
			hi[k] = e->maxs[k] - mins[k];
		}
		src_box(&s, lo, hi);
		trace_src(&s, org, NULL, start, end, tr);
	}
	tr->ent = tr->frac < 1 || tr->startsolid ? (int16_t)e->index : -1;
}

void col_clip(const col_ent_t *e, const float start[3], const float mins[3], const float maxs[3],
	const float end[3], col_trace_t *tr)
{
	float d[3], o[3];
	int k;

	if (stored(e) && ent_frame_delta(e->index, d))
	{
		for (k = 0; k < 3; k++)
			o[k] = e->origin[k] + d[k];
		clip_at(e, o, start, mins, maxs, end, tr);
	}
	else
		clip_at(e, e->origin, start, mins, maxs, end, tr);
}

void col_set_hitbox_source(col_hitbox_fn fn)
{
	hitbox_fn = fn;
}

int col_trace_hitboxes(const col_hitbox_t *hb, int n, const float start[3], const float end[3],
	col_trace_t *tr)
{
	col_trace_t best, t;
	src_t s;
	int i, k, hit = -1;

	/* each hitbox is the six-plane box tree of 4.3 in its bone frame,
	 * walked like a hull (GoldSrc traces one box hull per hitbox): the
	 * walk's plane epsilon decides grazing hits, and the reported plane
	 * is the hit face's [probe:measured T-hitbox]; the nearest impact
	 * wins, the first on a tie */
	memset(&best, 0, sizeof(best));
	best.frac = 2;
	for (i = 0; i < n; i++)
	{
		const col_hitbox_t *b = &hb[i];
		float ls[3], le[3];

		/* bone frame: R^T (p - t) */
		for (k = 0; k < 3; k++)
		{
			ls[k] = b->bone[0][k] * (start[0] - b->bone[0][3])
				+ b->bone[1][k] * (start[1] - b->bone[1][3])
				+ b->bone[2][k] * (start[2] - b->bone[2][3]);
			le[k] = b->bone[0][k] * (end[0] - b->bone[0][3])
				+ b->bone[1][k] * (end[1] - b->bone[1][3])
				+ b->bone[2][k] * (end[2] - b->bone[2][3]);
		}
		src_box(&s, b->mins, b->maxs);
		tr_init(&t, end);
		if (!walk_trace(&s, ls, le, &t) && !t.startsolid)
			continue;
		if (t.frac < best.frac || (t.startsolid && !best.startsolid))
		{
			float nb[3];

			best = t;
			hit = i;
			memcpy(nb, t.normal, sizeof(nb));
			for (k = 0; k < 3; k++)
				best.normal[k] = b->bone[k][0] * nb[0] + b->bone[k][1] * nb[1]
					+ b->bone[k][2] * nb[2];
			/* the plane's distance in the caller's frame */
			best.dist = t.dist + best.normal[0] * b->bone[0][3]
				+ best.normal[1] * b->bone[1][3] + best.normal[2] * b->bone[2][3];
		}
	}
	if (hit < 0)
		return 0;
	tr->frac = best.frac;
	tr->startsolid = best.startsolid;
	tr->allsolid = best.allsolid;
	tr->inopen = best.inopen;
	tr->inwater = best.inwater;
	for (k = 0; k < 3; k++)
	{
		tr->end[k] = start[k] + best.frac * (end[k] - start[k]);
		tr->normal[k] = best.normal[k];
	}
	tr->dist = best.dist;
	tr->hitgroup = hb[hit].group;
	return 1;
}

/* 4.5: a zero-size line trace whose box meets the linked box of a
 * studio-model box entity is traced against its hitboxes instead of its
 * bounding box (hitboxes reach past the box: arms at the box faces are hit
 * [probe:measured T-hitbox]); 0 when the entity has none (box stands).
 * The hitboxes are in e's frame; d = e's frame - the caller's. The source
 * gets the line in e's frame and may answer that it misses every hitbox
 * without posing them (-1: as a miss). */
static int trace_hitboxes(const col_ent_t *e, const float d[3], int moved, const float start[3],
	const float end[3], col_trace_t *t)
{
	const col_hitbox_t *hb;
	float s[3], en[3];
	int n, k;

	if (!hitbox_fn)
		return 0;
	for (k = 0; k < 3; k++)
	{
		s[k] = start[k] - d[k];
		en[k] = end[k] - d[k];
	}
	n = hitbox_fn(e, moved ? s : start, moved ? en : end, &hb);
	if (n == 0)
		return 0;
	tr_init(t, end);
	t->allsolid = 0;
	if (n < 0)
		return 1;
	if (!moved)
	{
		if (col_trace_hitboxes(hb, n, start, end, t))
			t->ent = (int16_t)e->index;
		return 1;
	}
	if (col_trace_hitboxes(hb, n, s, en, t))
	{
		t->ent = (int16_t)e->index;
		for (k = 0; k < 3; k++)
			t->end[k] += d[k];
		t->dist += t->normal[0] * d[0] + t->normal[1] * d[1] + t->normal[2] * d[2];
	}
	else
		memcpy(t->end, end, sizeof(t->end));
	return 1;
}

void col_move(const float start[3], const float mins[3], const float maxs[3], const float end[3],
	int type, const col_ent_t *pass, col_trace_t *tr)
{
	static const float m15[3] = { -15, -15, -15 }, p15[3] = { 15, 15, 15 };
	int mtype = type & 0xFF, n, i, k;
	int line = mins[0] == 0 && mins[1] == 0 && mins[2] == 0 && maxs[0] == 0 && maxs[1] == 0
		&& maxs[2] == 0;
	int mclip = pass && (pass->flags & COL_FL_MONSTERCLIP);
	float lo[3], hi[3], wend[3], wfrac;

	flq_tick();	/* a save's next flash page (flq.c): traces fill the AI's time */
	col_trace_world(start, mins, maxs, end, tr);
	/* the world stops it at the start (or it is all-solid): no entity is
	 * clipped (trace-and-hulls.md 4.4 step 1) [probe:measured P-push corpse] */
	if (tr->allsolid || tr->frac == 0)
		return;
	for (k = 0; k < 3; k++)
	{
		float bmin = mins[k], bmax = maxs[k];

		if (mtype == COL_MOVE_MISSILE)
		{
			bmin = bmin < -15 ? bmin : -15;
			bmax = bmax > 15 ? bmax : 15;
		}
		lo[k] = (start[k] < tr->end[k] ? start[k] : tr->end[k]) + bmin - 1;
		hi[k] = (start[k] < tr->end[k] ? tr->end[k] : start[k]) + bmax + 1;
	}
	/* entities are clipped against the part of the move the world left
	 * (start -> the world's end point); their fractions scale back to the
	 * whole move (an entity the start is stuck in reports all-solid at the
	 * world's end) [probe:measured T-segments] */
	copy3(wend, tr->end);
	wfrac = tr->frac;
	n = col_ents_box(lo, hi, cand, HL64_MAX_ENTITIES);
	st.cands += (uint32_t)n;
	for (i = 0; i < n; i++)
	{
		const col_ent_t *e = cand[i];
		col_trace_t t;
		float d[3];
		int moved;

		if (e->index == 0 || e->solid == COL_SOLID_NOT || e->solid == COL_SOLID_TRIGGER)
			continue;
		if (pass && (e == pass || e->index == pass->index))
			continue;
		if (mtype == COL_MOVE_NOMONSTERS && e->solid != COL_SOLID_BSP
			&& e->movetype != COL_MOVETYPE_PUSHSTEP)
			continue;	/* pushables stay solid [probe:measured T-segments] */
		if ((type & COL_MOVE_NOGLASS) && e->rendermode != COL_RENDER_NORMAL)
			continue;
		if (pass && pass->maxs[0] - pass->mins[0] != 0 && e->maxs[0] - e->mins[0] == 0)
			continue;		/* a box never meets a point entity */
		if (pass && (e->owner == pass->index || pass->owner == e->index))
			continue;
		/* func_monsterclip stops monsters only; the flag on a monster
		 * (hits monster clips) leaves it solid (trace-and-hulls.md 4.4) */
		if ((e->flags & COL_FL_MONSTERCLIP) && e->solid == COL_SOLID_BSP && !mclip)
			continue;
		/* candidates of another layer: into this frame */
		moved = ent_frame_delta(e->index, d);
		if (lo[0] > e->absmax[0] + d[0] || lo[1] > e->absmax[1] + d[1]
			|| lo[2] > e->absmax[2] + d[2] || hi[0] < e->absmin[0] + d[0]
			|| hi[1] < e->absmin[1] + d[1] || hi[2] < e->absmin[2] + d[2])
			continue;
		if (line && e->studio != COL_NONE
			&& (e->solid == COL_SOLID_BBOX || e->solid == COL_SOLID_SLIDEBOX)
			&& trace_hitboxes(e, d, moved, start, wend, &t))
			;
		else if (mtype == COL_MOVE_MISSILE && (e->flags & COL_FL_MONSTER))
			col_clip(e, start, m15, p15, wend, &t);
		else
			col_clip(e, start, mins, maxs, wend, &t);
		t.frac *= wfrac;
		if (t.allsolid || t.startsolid || t.frac < tr->frac)
		{
			int ss = tr->startsolid;

			*tr = t;
			if (ss)
				tr->startsolid = 1;
		}
		else if (t.startsolid)
			tr->startsolid = 1;
	}
}

void col_trace_line(const float start[3], const float end[3], int type, const col_ent_t *pass,
	col_trace_t *tr)
{
	col_move(start, zero3, zero3, end, type, pass, tr);
	if (tr->ent < 0)
		tr->ent = 0;
}

/* ---- surface materials (4.7) ------------------------------------------------------ */

static int mat_code(const wld_layer_t *L, uint32_t ci, uint32_t li, const float n[3],
	const float p[3])
{
	const w64_matmix_t *mx;
	int lo, hi, code = 0, dom, a0, a1;
	uint32_t gid, first, cnt, i;
	float bestd = 0;
	int best = -1;

	if (!L->mat)
		return 0;
	lo = L->mat_start[ci];
	hi = L->mat_start[ci + 1] - 1;
	while (lo <= hi)
	{
		int mid = (lo + hi) / 2;
		uint32_t e = L->mat_ent[mid], el = e >> 4;

		if (el == li)
		{
			code = (int)(e & 15);
			break;
		}
		if (el < li)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	if (code != MAT_MIXED)
		return code;
	gid = ci << W64_GID_LOCAL_BITS | li;
	lo = 0;
	hi = L->mat->nmix - 1;
	mx = NULL;
	while (lo <= hi)
	{
		int mid = (lo + hi) / 2;

		if (L->mat_mix[mid].gid == gid)
		{
			mx = &L->mat_mix[mid];
			break;
		}
		if (L->mat_mix[mid].gid < gid)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	if (!mx)
		return 0;
	/* the two axes other than the plane's dominant one, lower first */
	dom = 0;
	if (fabsf(n[1]) > fabsf(n[dom]))
		dom = 1;
	if (fabsf(n[2]) > fabsf(n[dom]))
		dom = 2;
	a0 = dom == 0 ? 1 : 0;
	a1 = dom == 2 ? 1 : 2;
	first = mx->first;
	cnt = mx->count;
	for (i = first; i < first + cnt; i++)
	{
		const w64_matface_t *f = &L->mat_face[i];
		float dx = p[a0] < f->lo[0] ? f->lo[0] - p[a0] : p[a0] > f->hi[0] ? p[a0] - f->hi[0] : 0;
		float dy = p[a1] < f->lo[1] ? f->lo[1] - p[a1] : p[a1] > f->hi[1] ? p[a1] - f->hi[1] : 0;
		float d = dx * dx + dy * dy;

		if (best < 0 || d < bestd)
		{
			best = (int)i;
			bestd = d;
			if (d == 0)
				break;
		}
	}
	return best < 0 ? 0 : L->mat_face[best].mat;
}

int col_material(uint16_t model, const float origin[3], const float angles[3],
	const float start[3], const float end[3])
{
	src_t s;
	col_trace_t t;
	float n[3], d;
	int code;

	if (!src_model(&s, model, 0))
		bad_model(model);
	trace_src(&s, origin, angles, start, end, &t);
	if (t.frac >= 1 || !s.hit_cp)
		return 0;
	plane_get(s.hit_cp, s.hit_li, n, &d);
	code = mat_code(s.L, s.hit_ci, s.hit_li, n, s.hit_p);
	if (dbg_trace())
	{
		const wld_layer_t *L = s.L;
		uint32_t h = 2166136261u, i, st0 = 0, st1 = 0;

		if (L->mat)
		{
			const uint16_t *v = L->mat_ent;

			/* values, not bytes: the host's tables are byte-swapped */
			for (i = 0; i < L->mat->nent; i++)
				h = (h ^ v[i]) * 16777619u;
			v = (const uint16_t *)L->mat_face;
			for (i = 0; i < 5u * L->mat->nface; i++)
				h = (h ^ v[i]) * 16777619u;
			st0 = L->mat_start[s.hit_ci];
			st1 = L->mat_start[s.hit_ci + 1];
		}
		log_printf("trace %lu material model=%u ci=%lu li=%lu p=%.9g,%.9g,%.9g n=%.9g,%.9g,%.9g "
			"mat=%d code=%d st=%lu..%lu ent0=%u hash=%08lx\n", (unsigned long)dbg_trace_frame(),
			(unsigned)model, (unsigned long)s.hit_ci, (unsigned long)s.hit_li,
			(double)s.hit_p[0], (double)s.hit_p[1], (double)s.hit_p[2], (double)n[0],
			(double)n[1], (double)n[2], L->mat ? 1 : 0, code, (unsigned long)st0,
			(unsigned long)st1, L->mat && st1 > st0 ? (unsigned)L->mat_ent[st0] : 0u,
			(unsigned long)h);
	}
	return code < (int)sizeof(mat_chars) - 1 ? mat_chars[code] : 'C';
}

/* ---- residency (architecture.md 5.3) ------------------------------------------------ */

/* The brush models a global box touches: solid ones and triggers (a touch
 * tests the mover against the trigger brush's hull: engine/phys/touch.c)
 * from the grid (a query in the caller's frame), contents ones here
 * (their linked boxes + g_M); slot << 16 | submodel, at most max. */
int col_models_box(const float lo[3], const float hi[3], uint32_t *out, int max)
{
	const float *f = eng_layer_origin(eng_frame_layer());
	float flo[3], fhi[3];
	int k, n, i, nm = 0;

	for (k = 0; k < 3; k++)
	{
		flo[k] = lo[k] - f[k];
		fhi[k] = hi[k] - f[k];
	}
	n = col_ents_box(flo, fhi, cand, HL64_MAX_ENTITIES);
	for (i = 0; i < n && nm < max; i++)
		if ((cand[i]->solid == COL_SOLID_BSP || cand[i]->solid == COL_SOLID_TRIGGER)
			&& cand[i]->model != COL_NONE && wld_layer(COL_MODEL_SLOT(cand[i]->model)))
			out[nm++] = (uint32_t)COL_MODEL_SLOT(cand[i]->model) << 16
				| COL_MODEL_SUB(cand[i]->model);
	for (i = 0; i < ncbrush && nm < max; i++)
	{
		const col_ent_t *e = cbrush[i];
		const float *gm = eng_layer_origin(ent_idc(e->index)->layer);

		for (k = 0; k < 3; k++)
			if (e->absmin[k] + gm[k] > hi[k] || e->absmax[k] + gm[k] < lo[k])
				break;
		if (k == 3 && wld_layer(COL_MODEL_SLOT(e->model)))
			out[nm++] = (uint32_t)COL_MODEL_SLOT(e->model) << 16 | COL_MODEL_SUB(e->model);
	}
	return nm;
}

/* The collision window: the player box + COL_WINDOW (global) and the
 * brush models it touches go to the stream as target-set inputs; the
 * stream names their COLL chunks (OPEN, + v * 0.5 s prefetch) only when
 * the window's grid cells or the model set changed. */
void col_want(const float mins[3], const float maxs[3])
{
	static uint32_t models[64];
	float lo[3], hi[3];
	int k;

	for (k = 0; k < 3; k++)
	{
		lo[k] = mins[k] - COL_WINDOW;
		hi[k] = maxs[k] + COL_WINDOW;
	}
	wld_stream_coll(lo, hi, models, col_models_box(lo, hi, models, 64));
}

void col_stats(col_stats_t *s)
{
	*s = st;
	memset(&st, 0, sizeof(st));
}

void col_counts(col_stats_t *s)
{
	*s = st;
}

/* ---- saved games (architecture.md 14) ------------------------------------------- */

/* The contents brushes by entity slot, in their order (col_contents
 * answers the first that holds the point). */
void col_contents_save(void)
{
	int i;

	sv_part("cbr");
	sv_tag(SV_TAG('C', 'B', 'R', '1'));
	sv_u32((uint32_t)ncbrush);
	for (i = 0; i < ncbrush; i++)
		sv_u16(cbrush[i]->index);
}

int col_contents_load(void)
{
	int i, n;

	if (sv_expect(SV_TAG('C', 'B', 'R', '1')))
		return -1;
	n = (int)sv_get_u32();
	if (n < 0 || n > COL_CBRUSH_MAX)
	{
		sv_fail("contents brushes");
		return -1;
	}
	ncbrush = 0;
	for (i = 0; i < n; i++)
	{
		uint32_t e = sv_get_u16();

		if (e >= HL64_MAX_ENTITIES)
		{
			sv_fail("contents brush slot");
			return -1;
		}
		cbrush[ncbrush++] = ent_cv((int)e);
	}
	return sv_failed() ? -1 : 0;
}
