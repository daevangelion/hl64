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
#include "engine/core/log.h"
#include "engine/core/sys.h"
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

/* A hull to walk: a layer's COLL tree, or the box tree. */
typedef struct {
	wld_layer_t	*L;		/* NULL: box tree */
	const uint8_t	*box;		/* box tree chunk */
	int32_t		root;		/* gid, or a leaf child */
	uint8_t		hull0;		/* leaves are BSP leaf indices (hull 0) */
	/* chunk cache */
	uint32_t	ci;
	const uint8_t	*cp;
	/* impact node (materials) and its crossing point, model frame */
	const uint8_t	*hit_cp;
	uint32_t	hit_ci, hit_li;
	float		hit_p[3];
} src_t;

typedef struct {
	int32_t		far;		/* far child: gid or leaf child */
	const uint8_t	*cp;		/* the split node's chunk */
	uint32_t	ci;
	uint16_t	li;		/* the split node */
	uint8_t		side;		/* 1: the sub-segment starts behind the plane */
	uint8_t		pad;
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

/* The COLL chunk of a node: cached, resident, or read now. */
static const uint8_t *chunk_of(src_t *s, uint32_t ci)
{
	wld_layer_t *L = s->L;
	const uint8_t *cp;

	if (!L)
		return s->box;
	if (s->cp && ci == s->ci)
		return s->cp;
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
	return cp;
}

#define HDR(cp)		((const w64_coll_t *)(cp))
#define NODES(cp)	((const w64_cnode_t *)((cp) + 16 + 16 * HDR(cp)->nplanes))
#define PLANES(cp)	((const w64_cplane_t *)((cp) + 16))

static const w64_cnode_t *node_of(src_t *s, int32_t gid, const uint8_t **cpp, uint32_t *lip)
{
	uint32_t ci = W64_GID_COLL(gid), li = W64_GID_LOCAL(gid);
	const uint8_t *cp = chunk_of(s, ci);

	if (li >= HDR(cp)->nnodes)
		sys_error("coll: node %ld past its chunk", (long)gid);
	*cpp = cp;
	*lip = li;
	st.nodes++;
	return NODES(cp) + li;
}

/* Child c of a node in chunk ci -> gid or leaf child. */
static int32_t child_ref(const uint8_t *cp, uint32_t ci, int c)
{
	const w64_coll_t *h = HDR(cp);

	if (c < 0)
		return c;
	if (c < h->nnodes)
		return (int32_t)(ci << W64_GID_LOCAL_BITS | (uint32_t)c);
	return (int32_t)((const uint32_t *)(NODES(cp) + h->nnodes))[c - h->nnodes];
}

/* Plane of node li in chunk cp: axis 0-2 (unit normal) or 3 (pl). */
static float plane_dist(const uint8_t *cp, uint32_t li, const float p[3])
{
	const w64_coll_t *h = HDR(cp);
	const w64_cnode_t *nd = NODES(cp) + li;
	const w64_cplane_t *pl;

	if (li < h->nx)
		return p[0] - nd->u.dist;
	if (li < h->ny)
		return p[1] - nd->u.dist;
	if (li < h->nz)
		return p[2] - nd->u.dist;
	pl = PLANES(cp) + nd->u.plane;
	return (float)((double)pl->normal[0] * p[0] + (double)pl->normal[1] * p[1]
		+ (double)pl->normal[2] * p[2] - pl->dist);
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

static int walk_point(src_t *s, int32_t ref, const float p[3])
{
	int steps;

	for (steps = 0; ref >= 0; steps++)
	{
		const uint8_t *cp;
		uint32_t li;
		const w64_cnode_t *nd = node_of(s, ref, &cp, &li);

		if (steps == COL_WALK_MAX)
			sys_error("coll: hull walk deeper than %d", COL_WALK_MAX);
		ref = child_ref(cp, s->ci, nd->child[plane_dist(cp, li, p) >= 0 ? 0 : 1]);
	}
	return leaf_contents(s, ref);
}

static int src_model(src_t *s, uint16_t model, int hull)
{
	unsigned slot = COL_MODEL_SLOT(model), sub = COL_MODEL_SUB(model);
	wld_layer_t *L = wld_layer((int)slot);

	if (!L || sub >= L->dir->nsub || hull < 0 || hull >= COL_HULLS)
		return 0;
	memset(s, 0, sizeof(*s));
	s->L = L;
	s->root = L->models[sub].head[hull];
	s->hull0 = hull == 0;
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
	memset(s, 0, sizeof(*s));
	s->box = box_tree;
	s->root = 0;
}

/* ---- segment walk (spec 3) -------------------------------------------------- */

static void lerp(float out[3], const float a[3], const float b[3], float f)
{
	out[0] = a[0] + f * (b[0] - a[0]);
	out[1] = a[1] + f * (b[1] - a[1]);
	out[2] = a[2] + f * (b[2] - a[2]);
}

/* Walks p1 -> p2 (model frame) from s->root. Flags accumulate in tr;
 * returns 1 with tr->frac / end / normal / dist (model frame) on an
 * impact. tr->allsolid must start at 1. */
static int walk_trace(src_t *s, const float p1[3], const float p2[3], col_trace_t *tr)
{
	float a[3], b[3], af = 0, bf = 1;
	int32_t ref = s->root;
	int sp = 0;

	memcpy(a, p1, sizeof(a));
	memcpy(b, p2, sizeof(b));
	for (;;)
	{
		int c;

		while (ref >= 0)
		{
			const uint8_t *cp;
			uint32_t li;
			const w64_cnode_t *nd = node_of(s, ref, &cp, &li);
			float t1 = plane_dist(cp, li, a), t2 = plane_dist(cp, li, b), frac;
			split_t *fr;
			int side;

			if (t1 >= 0 && t2 >= 0)
			{
				ref = child_ref(cp, s->ci, nd->child[0]);
				continue;
			}
			if (t1 < 0 && t2 < 0)
			{
				ref = child_ref(cp, s->ci, nd->child[1]);
				continue;
			}
			side = t1 < 0;
			frac = side ? (t1 + COL_EPS) / (t1 - t2) : (t1 - COL_EPS) / (t1 - t2);
			if (frac < 0)
				frac = 0;
			if (frac > 1)
				frac = 1;
			if (sp == COL_STACK)
				sys_error("coll: hull walk deeper than %d splits", COL_STACK);
			fr = &stack[sp++];
			fr->far = child_ref(cp, s->ci, nd->child[side ^ 1]);
			fr->cp = cp;
			fr->ci = s->ci;
			fr->li = (uint16_t)li;
			fr->side = (uint8_t)side;
			fr->frac = frac;
			fr->p1f = af;
			fr->p2f = bf;
			memcpy(fr->p1, a, sizeof(a));
			memcpy(fr->p2, b, sizeof(b));
			ref = child_ref(cp, s->ci, nd->child[side]);
			bf = af + (bf - af) * frac;
			lerp(b, fr->p1, fr->p2, frac);
		}
		c = leaf_contents(s, ref);
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

		/* the near part ended without an impact: the far parts, innermost first */
		for (;;)
		{
			split_t *fr;
			float m[3], mf, frac, t1, t2;

			if (!sp)
				return 0;
			fr = &stack[--sp];
			frac = fr->frac;
			mf = fr->p1f + (fr->p2f - fr->p1f) * frac;
			lerp(m, fr->p1, fr->p2, frac);
			if (walk_point(s, fr->far, m) != COL_SOLID)
			{
				ref = fr->far;
				memcpy(a, m, sizeof(a));
				af = mf;
				memcpy(b, fr->p2, sizeof(b));
				bf = fr->p2f;
				break;
			}
			if (tr->allsolid)
				return 0;		/* never left the solid */
			s->cp = fr->cp;			/* the frame's chunk stays resident (used this frame) */
			s->ci = fr->ci;
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
			memcpy(tr->end, m, sizeof(m));
			return 1;
		}
	}
}

/* ---- frames ---------------------------------------------------------------- */

static void angle_vectors(const float ang[3], float f[3], float r[3], float u[3])
{
	const float k = 3.14159265358979323846f / 180.0f;
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

static int rotated(const float *ang)
{
	return ang && (ang[0] != 0 || ang[1] != 0 || ang[2] != 0);
}

static int finite3(const float v[3])
{
	return isfinite(v[0]) && isfinite(v[1]) && isfinite(v[2]);
}

static void tr_init(col_trace_t *tr, const float end[3])
{
	memset(tr, 0, sizeof(*tr));
	tr->frac = 1;
	tr->allsolid = 1;
	tr->ent = -1;
	memcpy(tr->end, end, sizeof(tr->end));
}

/* Trace s placed at o (rotated by ang about o when non-zero): spec 4.2. */
static void trace_src(src_t *s, const float o[3], const float *ang, const float start[3],
	const float end[3], col_trace_t *tr)
{
	float ls[3], le[3], f[3], r[3], u[3];
	int k, rot = rotated(ang);

	tr_init(tr, end);
	st.traces++;
	if (!finite3(start) || !finite3(end) || !finite3(o))
	{
		if (!warned_nan)
			log_printf("coll: non-finite trace input: start solid\n");
		warned_nan = 1;
		tr->startsolid = 1;
		return;
	}
	for (k = 0; k < 3; k++)
	{
		ls[k] = start[k] - o[k];
		le[k] = end[k] - o[k];
	}
	if (rot)
	{
		float a[3], b[3];

		angle_vectors(ang, f, r, u);
		memcpy(a, ls, sizeof(a));
		memcpy(b, le, sizeof(b));
		ls[0] = a[0] * f[0] + a[1] * f[1] + a[2] * f[2];
		ls[1] = -(a[0] * r[0] + a[1] * r[1] + a[2] * r[2]);
		ls[2] = a[0] * u[0] + a[1] * u[1] + a[2] * u[2];
		le[0] = b[0] * f[0] + b[1] * f[1] + b[2] * f[2];
		le[1] = -(b[0] * r[0] + b[1] * r[1] + b[2] * r[2]);
		le[2] = b[0] * u[0] + b[1] * u[1] + b[2] * u[2];
	}
	if (!walk_trace(s, ls, le, tr))
	{
		memcpy(tr->end, end, sizeof(tr->end));
		return;
	}
	if (rot)
	{
		float n[3];

		memcpy(n, tr->normal, sizeof(n));
		for (k = 0; k < 3; k++)
		{
			tr->normal[k] = n[0] * f[k] - n[1] * r[k] + n[2] * u[k];
			tr->end[k] = start[k] + tr->frac * (end[k] - start[k]);
		}
	}
	else
		for (k = 0; k < 3; k++)
			tr->end[k] += o[k];
}

/* ---- frames (architecture.md 8) ---------------------------------------------- */

/* Origin of layer L in the frame of eng_frame_layer(): g_M(L) - g_M(frame)
 * (the frame's own layer: exactly 0). */
static void layer_in_frame(const wld_layer_t *L, float o[3])
{
	const float *f = eng_layer_origin(eng_frame_layer());
	int k;

	for (k = 0; k < 3; k++)
		o[k] = L->gofs[k] - f[k];
}

/* A collision view in the entity layer's storage (else a caller's copy,
 * pmove physents: already in the caller's frame). */
static int stored(const col_ent_t *e)
{
	return e->index < HL64_MAX_ENTITIES && e == ent_cv(e->index);
}

/* ---- contents ------------------------------------------------------------------ */

static int layer_has(const wld_layer_t *L, const float o[3], const float p[3], const float lo[3],
	const float hi[3])
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
	int slot, c = COL_SOLID;

	if (!finite3(p))
		return COL_SOLID;
	for (slot = 0; slot < HL64_WORLD_LAYERS && c == COL_SOLID; slot++)
	{
		wld_layer_t *L = wld_layer(slot);
		float lp[3], o[3];

		if (!L)
			continue;
		layer_in_frame(L, o);
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

int col_model_bounds(uint16_t model, float mins[3], float maxs[3])
{
	wld_layer_t *L = wld_layer(COL_MODEL_SLOT(model));
	unsigned sub = COL_MODEL_SUB(model);

	if (!L || sub >= L->dir->nsub)
		return 0;
	memcpy(mins, L->models[sub].mins, 3 * sizeof(float));
	memcpy(maxs, L->models[sub].maxs, 3 * sizeof(float));
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

void col_trace_world(const float start[3], const float mins[3], const float maxs[3],
	const float end[3], col_trace_t *tr)
{
	int hull = col_hull_for_size(mins, maxs), slot, any = 0, k;
	float lo[3], hi[3];

	for (k = 0; k < 3; k++)
	{
		lo[k] = (start[k] < end[k] ? start[k] : end[k]) + mins[k];
		hi[k] = (start[k] < end[k] ? end[k] : start[k]) + maxs[k];
	}
	tr_init(tr, end);
	for (slot = 0; slot < HL64_WORLD_LAYERS; slot++)
	{
		wld_layer_t *L = wld_layer(slot);
		col_trace_t t;
		src_t s;
		float o[3], lo3[3];

		if (!L)
			continue;
		layer_in_frame(L, lo3);
		if (!layer_has(L, lo3, zero3, lo, hi))
			continue;
		src_model(&s, COL_MODEL(slot, 0), hull);
		hull_place(o, lo3, hull, mins);
		trace_src(&s, o, NULL, start, end, &t);
		if (t.frac < 1)
			t.dist += t.normal[0] * lo3[0] + t.normal[1] * lo3[1] + t.normal[2] * lo3[2];
		if (!any || nearer(&t, tr))
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
 * The hitboxes are in e's frame; d = e's frame - the caller's. */
static int trace_hitboxes(const col_ent_t *e, const float d[3], int moved, const float start[3],
	const float end[3], col_trace_t *t)
{
	const col_hitbox_t *hb;
	int n, k;

	if (!hitbox_fn || (n = hitbox_fn(e, &hb)) <= 0)
		return 0;
	tr_init(t, end);
	t->allsolid = 0;
	if (!moved)
	{
		if (col_trace_hitboxes(hb, n, start, end, t))
			t->ent = (int16_t)e->index;
		return 1;
	}
	{
		float s[3], en[3];

		for (k = 0; k < 3; k++)
		{
			s[k] = start[k] - d[k];
			en[k] = end[k] - d[k];
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
	}
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
	memcpy(wend, tr->end, sizeof(wend));
	wfrac = tr->frac;
	n = col_ents_box(lo, hi, cand, HL64_MAX_ENTITIES);
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
		if ((e->flags & COL_FL_MONSTERCLIP) && !mclip)
			continue;		/* func_monsterclip stops monsters only */
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
	return code < (int)sizeof(mat_chars) - 1 ? mat_chars[code] : 'C';
}

/* ---- residency (architecture.md 5.3) ------------------------------------------------ */

static void want_model(wld_layer_t *L, unsigned sub)
{
	const w64_model_t *m;
	unsigned c;

	if (sub >= L->dir->nsub)
		return;
	m = &L->models[sub];
	for (c = 0; c < m->coll_count && m->coll_first + c < L->dir->ncoll; c++)
		wld_want_coll(L, L->dir->coll_first + (int)(m->coll_first + c));
}

void col_want(const float mins[3], const float maxs[3])
{
	float lo[3], hi[3];
	int slot, k, n, i;

	for (k = 0; k < 3; k++)
	{
		lo[k] = mins[k] - COL_WINDOW;
		hi[k] = maxs[k] + COL_WINDOW;
	}
	for (slot = 0; slot < HL64_WORLD_LAYERS; slot++)
	{
		wld_layer_t *L = wld_layer(slot);
		const w64_grid_t *g;
		int c0[3], c1[3], x, y, z;

		if (!L)
			continue;
		if (!++L->coll_gen)
			L->coll_gen = 1;
		g = L->grid;
		for (k = 0; k < 3; k++)
		{
			float a = (lo[k] - L->gofs[k] - g->origin[k]) / (float)g->cell;
			float b = (hi[k] - L->gofs[k] - g->origin[k]) / (float)g->cell;

			c0[k] = a < 0 ? 0 : a >= g->dims[k] ? g->dims[k] : (int)a;
			c1[k] = b < 0 ? -1 : b >= g->dims[k] ? g->dims[k] - 1 : (int)b;
		}
		for (z = c0[2]; z <= c1[2]; z++)
			for (y = c0[1]; y <= c1[1]; y++)
				for (x = c0[0]; x <= c1[0]; x++)
				{
					const uint16_t *v = L->glists
						+ L->cells[x + g->dims[0] * (y + g->dims[1] * z)].list;

					for (i = 1; i <= v[0]; i++)
						wld_want_coll(L, L->dir->coll_first + v[i]);
				}
	}
	/* brushes the window touches: solid ones from the grid (a query in the
	 * caller's frame), contents ones here (their linked boxes + g_M) */
	{
		const float *f = eng_layer_origin(eng_frame_layer());
		float flo[3], fhi[3];

		for (k = 0; k < 3; k++)
		{
			flo[k] = lo[k] - f[k];
			fhi[k] = hi[k] - f[k];
		}
		n = col_ents_box(flo, fhi, cand, HL64_MAX_ENTITIES);
	}
	for (i = 0; i < n; i++)
		if (cand[i]->solid == COL_SOLID_BSP && cand[i]->model != COL_NONE
			&& wld_layer(COL_MODEL_SLOT(cand[i]->model)))
			want_model(wld_layer(COL_MODEL_SLOT(cand[i]->model)), COL_MODEL_SUB(cand[i]->model));
	for (i = 0; i < ncbrush; i++)
	{
		const col_ent_t *e = cbrush[i];
		const float *gm = eng_layer_origin(ent_idc(e->index)->layer);

		for (k = 0; k < 3; k++)
			if (e->absmin[k] + gm[k] > hi[k] || e->absmax[k] + gm[k] < lo[k])
				break;
		if (k == 3 && wld_layer(COL_MODEL_SLOT(e->model)))
			want_model(wld_layer(COL_MODEL_SLOT(e->model)), COL_MODEL_SUB(e->model));
	}
}

void col_stats(col_stats_t *s)
{
	*s = st;
	memset(&st, 0, sizeof(st));
}
