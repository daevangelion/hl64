/* SPDX-License-Identifier: MIT */
/*
 * hl_nodecook.h — GoldSrc node graph cook: the info_node / info_node_air
 * entities of a BSP30 map → one position-independent "HLN1" graph
 * (docs/formats/hle-format-v1.md § Node graph), appended to the map's HLE1
 * entity chunk. The graph build itself is HLSDK dlls/nodes.cpp
 * CTestHull::BuildNodeGraph run offline against the BSP's own hulls (a
 * GoldSrc game builds the same graph at its first load of the map and
 * caches it as maps/graphs/MAP.nod); it lives under the Half Life 1 SDK
 * licence in tools/hlsdk/hl_nodegraph.h and is included below:
 *
 *   nodes     entity order; land nodes dropped to the floor (traceline 384
 *             down, world + solid brush entities) and lifted NODE_HEIGHT for
 *             linking, water nodes by point contents, info_node_air as is
 *   links     LinkVisibleNodes: every same-realm pair whose traceline is
 *             clear, or blocked by one brush entity both ways (link entity);
 *             walk rejection per hull (small / human / large: WALKMOVE in
 *             16-unit steps from node to node over the world hulls, ending
 *             within 64 units; fly: a large hull trace); RejectInlineLinks
 *   output    land nodes pushed back down to the floor
 *
 * This file holds the format, the BSP30 reader and the engine services the
 * build calls: point contents, segment traces against the world and the
 * brush entities, the monster walk move and its ground check — written from
 * docs/specs/trace-and-hulls.md and docs/specs/physics-movetypes.md § 6.
 * Tracelines see the world and every SOLID_BSP brush entity but
 * func_monsterclip; the walk test's world-only moves (walkers' step and
 * ground check; swimmers move normally) see the world and the FL_WORLDBRUSH
 * entities, func_monsterclip included; the ground check traces from a step
 * above the box bottom.
 *
 * Deviations: brush entities are traced at their spawn position (origin
 * key; a linear func_door with START_OPEN at its open position; no
 * rotation); link entities other than doors and func_breakable are dropped
 * (HandleLinkEnt never lets a monster through them); no SortNodes
 * renumbering (node index = entity order; it only changes tie-breaks
 * between equal-length paths); hint type / activity are not kept (no
 * hint-node consumer). Check: the Uplink cook equals the shipped
 * maps/graphs/hldemo{1,2,3}.nod and t0a0d.nod link for link (hull bits,
 * link entities, node heights).
 *
 * Hostile input: hlnc_load rejects a BSP whose lumps, counts, plane, leaf,
 * child or head indices are out of range, or whose node trees have a cycle
 * or are deeper than HLNC_MAX_DEPTH (so every walk terminates and the trace
 * recursion is bounded).
 *
 * Pure C99, no engine dependencies. The runtime only needs the format part:
 * define HLN_FORMAT_ONLY first. The cook part needs hl_entcook.h's cook
 * included before it (hle_cook_bsp builds both chunks).
 */
#ifndef HL_NODECOOK_H
#define HL_NODECOOK_H

#include <stdint.h>
#include <string.h>

#define HLN_MAGIC	0x314E4C48u	/* bytes "HLN1" read as a host u32 (LE) */
#define HLN_MAGIC_SW	0x484C4E31u
#define HLN_VERSION	1
#define HLN_F_BIGENDIAN	1
#define HLN_MAX_NODES	1024		/* HLSDK MAX_NODES */

/* node info (HLSDK nodes.h bits_NODE_*) */
#define HLN_NODE_LAND	1
#define HLN_NODE_AIR	2
#define HLN_NODE_WATER	4
/* link hulls (bits_LINK_*_HULL) */
#define HLN_LINK_SMALL	1
#define HLN_LINK_HUMAN	2
#define HLN_LINK_LARGE	4
#define HLN_LINK_FLY	8
/* link entity (hln_link_t.lent): kind in the low bits, door spawnflags */
#define HLN_LE_KIND		3
#define HLN_LE_NONE		0
#define HLN_LE_DOOR		1	/* func_door / func_door_rotating */
#define HLN_LE_BREAKABLE	2	/* func_breakable */
#define HLN_LE_USEONLY		4	/* SF_DOOR_USE_ONLY */
#define HLN_LE_NOAUTORETURN	8	/* SF_DOOR_NO_AUTO_RETURN */
#define HLN_LE_NOMONSTERS	16	/* SF_DOOR_NOMONSTERS */

typedef struct
{
	uint32_t	magic;
	uint16_t	version, flags;
	uint32_t	size;		/* whole section, bytes */
	uint16_t	numnodes, numlinks;
	uint32_t	ofs_nodes, ofs_links;
} hln_header_t;			/* 24 bytes */

typedef struct
{
	float		origin[3];	/* map space; land nodes on the floor */
	uint16_t	first_link;
	uint8_t		num_links;	/* < MAX_NODE_INITIAL_LINKS (128) */
	uint8_t		info;		/* HLN_NODE_* */
} hln_node_t;			/* 16 bytes */

typedef struct
{
	uint16_t	dest;		/* node index */
	uint8_t		hulls;		/* HLN_LINK_* */
	uint8_t		lent;		/* HLN_LE_* of the brush entity in the way */
	uint16_t	lmodel;		/* its BSP model ("*N"), 0 none */
} hln_link_t;			/* 6 bytes; weight = 2D node distance */

static inline int hln_host_big (void)
{
	const uint16_t	one = 1;

	return *(const uint8_t *)&one == 0;
}

static inline uint16_t hln_sw16 (uint16_t v)
{
	return (uint16_t)((v >> 8) | (v << 8));
}

static inline uint32_t hln_sw32 (uint32_t v)
{
	return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u)
		| (v << 24);
}

static inline void hln_swap_header (hln_header_t *h)
{
	h->magic = hln_sw32 (h->magic);
	h->version = hln_sw16 (h->version);
	h->flags = hln_sw16 (h->flags);
	h->size = hln_sw32 (h->size);
	h->numnodes = hln_sw16 (h->numnodes);
	h->numlinks = hln_sw16 (h->numlinks);
	h->ofs_nodes = hln_sw32 (h->ofs_nodes);
	h->ofs_links = hln_sw32 (h->ofs_links);
}

/* swap nodes and links (header already host order) */
static inline void hln_swap_body (uint8_t *b, const hln_header_t *h)
{
	uint32_t	i, k;

	for (i = 0; i < h->numnodes; i++)
	{
		hln_node_t	*n = (hln_node_t *)(b + h->ofs_nodes) + i;
		uint32_t	*f = (uint32_t *)n->origin;

		for (k = 0; k < 3; k++)
			f[k] = hln_sw32 (f[k]);
		n->first_link = hln_sw16 (n->first_link);
	}
	for (i = 0; i < h->numlinks; i++)
	{
		hln_link_t	*l = (hln_link_t *)(b + h->ofs_links) + i;

		l->dest = hln_sw16 (l->dest);
		l->lmodel = hln_sw16 (l->lmodel);
	}
}

/*
 * Validate a graph and bring it to host byte order in place (idempotent).
 * Returns 0 on success, else a reason. Hostile-input safe.
 */
static inline const char *hln_fixup (void *blob, uint32_t len)
{
	uint8_t		*b = blob;
	hln_header_t	*h = blob;
	uint32_t	i;
	int		big;

	if (!blob || len < sizeof (*h) || ((uintptr_t)blob & 3))
		return "short or unaligned graph";
	if (h->magic == HLN_MAGIC_SW)
	{
		hln_swap_header (h);
		big = !hln_host_big ();
	}
	else if (h->magic == HLN_MAGIC)
		big = hln_host_big ();
	else
		return "bad magic";
	if (h->version != HLN_VERSION)
		return "unsupported version";
	if (((h->flags & HLN_F_BIGENDIAN) != 0) != big)
		return "byte-order flag mismatch";
	if (h->size > len || h->size < sizeof (*h))
		return "truncated";
	if (!h->numnodes || h->numnodes > HLN_MAX_NODES
		|| h->ofs_nodes > h->size || (h->ofs_nodes & 3)
		|| h->numnodes > (h->size - h->ofs_nodes) / sizeof (hln_node_t)
		|| h->ofs_links > h->size || (h->ofs_links & 1)
		|| h->numlinks > (h->size - h->ofs_links) / sizeof (hln_link_t))
		return "section out of range";
	if (big != hln_host_big ())
		hln_swap_body (b, h);
	h->flags = (uint16_t)((h->flags & ~HLN_F_BIGENDIAN)
		| (hln_host_big () ? HLN_F_BIGENDIAN : 0));
	for (i = 0; i < h->numnodes; i++)
	{
		const hln_node_t *n = (const hln_node_t *)(b + h->ofs_nodes) + i;

		if ((uint32_t)n->first_link + n->num_links > h->numlinks)
			return "node links out of range";
	}
	for (i = 0; i < h->numlinks; i++)
		if (((const hln_link_t *)(b + h->ofs_links))[i].dest >= h->numnodes)
			return "link out of range";
	return 0;
}

#ifndef HLN_FORMAT_ONLY
/*================================================================ cook */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define HLNC_NODE_HEIGHT	8	/* nodes.cpp NODE_HEIGHT */
#define HLNC_HULL_STEP		16	/* HULL_STEP_SIZE */
#define HLNC_MAX_INITIAL	128	/* MAX_NODE_INITIAL_LINKS */
#define HLNC_STEPSIZE		18	/* physics-movetypes.md 1: step height */
#define HLNC_DIST_EPSILON	0.03125f
#define HLNC_CONTENTS_EMPTY	-1
#define HLNC_CONTENTS_SOLID	-2
#define HLNC_CONTENTS_WATER	-3
#define HLNC_CONTENTS_CURRENT_0	-9
#define HLNC_CONTENTS_CURRENT_DOWN -14

typedef struct { float n[3], d; int type; } hlnc_plane_t;
typedef struct { int plane, child[2]; } hlnc_cnode_t;	/* child < 0: contents */
typedef struct { float mins[3], maxs[3]; int head[4]; } hlnc_model_t;

typedef struct
{
	int	model;		/* BSP model index */
	float	org[3];
	float	mins[3], maxs[3];	/* absolute bounds at spawn */
	int	lent;		/* HLN_LE_* (0 kind: not a usable link entity) */
	int	wbrush;		/* FL_WORLDBRUSH: part of the world for world-only moves */
	int	mclip;		/* FL_MONSTERCLIP: skipped by the other traces */
} hlnc_solid_t;

typedef struct
{
	hlnc_plane_t	*planes;
	int		nplanes;
	hlnc_cnode_t	*hull0, *clip;	/* BSP nodes (leaf → contents) / clipnodes */
	int		nhull0, nclip;
	hlnc_model_t	*models;
	int		nmodels;
	hlnc_solid_t	*solids;	/* solid brush entities */
	int		nsolids;
} hlnc_bsp_t;

typedef struct
{
	int	allsolid, startsolid;
	float	fraction;
	float	endpos[3];
	int	ent;		/* -1 none, -2 world, else hlnc_bsp_t.solids index */
} hlnc_trace_t;

typedef struct
{
	int	nodes, links, visible, rejected_walk, rejected_inline;
} hln_stats_t;

/* GoldSrc hull clip boxes (hull 1 human, 2 large, 3 head) */
static const float hlnc_clip_mins[4][3] = {
	{ 0, 0, 0 }, { -16, -16, -36 }, { -32, -32, -32 }, { -16, -16, -18 }
};

static uint32_t hlnc_rd32 (const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16
		| (uint32_t)p[3] << 24;
}

static int16_t hlnc_rd16 (const uint8_t *p)
{
	return (int16_t)(uint16_t)(p[0] | p[1] << 8);
}

static float hlnc_rdf (const uint8_t *p)
{
	uint32_t	u = hlnc_rd32 (p);
	float		f;

	memcpy (&f, &u, 4);
	return f;
}

/*---- engine services (docs/specs/trace-and-hulls.md, physics-movetypes.md) */

/* one hull tree of the map: hull 0 = BSP nodes (leaves already turned into
 * contents by hlnc_load), hulls 1-3 = clipnodes */
typedef struct
{
	const hlnc_bsp_t	*b;
	const hlnc_cnode_t	*nodes;
	int			 root;
} hlnc_hull_t;

/* signed distance of p to a node's plane (axial planes: one coordinate) */
static float hlnc_pdist (const hlnc_plane_t *pl, const float *p)
{
	if (pl->type < 3)
		return p[pl->type] - pl->d;
	return pl->n[0] * p[0] + pl->n[1] * p[1] + pl->n[2] * p[2] - pl->d;
}

/* contents of the leaf holding p (spec 1: >= 0 front, < 0 back) */
static int hlnc_pc (const hlnc_hull_t *h, int num, const float *p)
{
	while (num >= 0)
	{
		const hlnc_cnode_t *n = &h->nodes[num];

		num = n->child[hlnc_pdist (&h->b->planes[n->plane], p) < 0];
	}
	return num;
}

/*
 * Segment p1 → p2 (fractions f1 → f2 of the whole trace) through the
 * subtree num (spec 3). Returns 0 once the impact is found. tr starts
 * all-solid with fraction 1; a non-solid leaf clears all-solid, a solid one
 * marks start-solid. The impact is the first crossing from a non-solid side
 * into SOLID; its point stands DIST_EPSILON off the plane on the near side
 * and backs off by tenths of the sub-segment while it still classifies
 * SOLID. Depth is bounded by hlnc_load (hlnc_tree_check).
 */
static int hlnc_rhc (const hlnc_hull_t *h, int num, float f1, float f2,
	const float *p1, const float *p2, hlnc_trace_t *tr)
{
	const hlnc_cnode_t	*n;
	const hlnc_plane_t	*pl;
	float			 t1, t2, frac, midf, mid[3];
	int			 k, near;

	if (num < 0)
	{
		if (num == HLNC_CONTENTS_SOLID)
			tr->startsolid = 1;
		else
			tr->allsolid = 0;
		return 1;
	}
	n = &h->nodes[num];
	pl = &h->b->planes[n->plane];
	t1 = hlnc_pdist (pl, p1);
	t2 = hlnc_pdist (pl, p2);
	if (t1 >= 0 && t2 >= 0)
		return hlnc_rhc (h, n->child[0], f1, f2, p1, p2, tr);
	if (t1 < 0 && t2 < 0)
		return hlnc_rhc (h, n->child[1], f1, f2, p1, p2, tr);

	/* the segment crosses the plane: split at the stand-off point */
	near = t1 < 0;
	if (near)
		frac = (t1 + HLNC_DIST_EPSILON) / (t1 - t2);
	else
		frac = (t1 - HLNC_DIST_EPSILON) / (t1 - t2);
	if (frac < 0)
		frac = 0;
	if (frac > 1)
		frac = 1;
	midf = f1 + (f2 - f1) * frac;
	for (k = 0; k < 3; k++)
		mid[k] = p1[k] + frac * (p2[k] - p1[k]);
	if (!hlnc_rhc (h, n->child[near], f1, midf, p1, mid, tr))
		return 0;
	if (hlnc_pc (h, n->child[!near], mid) != HLNC_CONTENTS_SOLID)
		return hlnc_rhc (h, n->child[!near], midf, f2, mid, p2, tr);
	if (tr->allsolid)
		return 0;		/* still inside the solid it started in */

	/* impact; keep the end point out of the solid */
	while (hlnc_pc (h, h->root, mid) == HLNC_CONTENTS_SOLID)
	{
		frac -= 0.1f;
		if (!(frac >= 0))
			break;		/* never back past the start */
		midf = f1 + (f2 - f1) * frac;
		for (k = 0; k < 3; k++)
			mid[k] = p1[k] + frac * (p2[k] - p1[k]);
	}
	tr->fraction = midf;
	memcpy (tr->endpos, mid, sizeof (mid));
	return 0;
}

/* the hull a box of this size collides as (spec 2) */
static int hlnc_hull_of (const float *mins, const float *maxs)
{
	float	sx = maxs[0] - mins[0];

	if (sx <= 8)
		return 0;
	if (sx <= 36)
		return maxs[2] - mins[2] <= 36 ? 3 : 1;
	return 2;
}

/*
 * Box mins..maxs moved start → end against BSP model `model` placed at org
 * (world: model 0 at the origin). The box is traced as a point through the
 * selected hull, offset o = org + (hull mins − box mins) (spec 4.2,
 * unrotated); an impact's end point is the local one moved back by o.
 */
static hlnc_trace_t hlnc_clip_model (const hlnc_bsp_t *b, int model,
	const float *org, const float *start, const float *mins,
	const float *maxs, const float *end)
{
	hlnc_hull_t	h;
	hlnc_trace_t	tr;
	float		o[3], s[3], e[3];
	int		hull = hlnc_hull_of (mins, maxs), k;

	h.b = b;
	h.nodes = hull ? b->clip : b->hull0;
	h.root = b->models[model].head[hull];
	for (k = 0; k < 3; k++)
	{
		o[k] = hlnc_clip_mins[hull][k] - mins[k] + org[k];
		s[k] = start[k] - o[k];
		e[k] = end[k] - o[k];
	}
	memset (&tr, 0, sizeof (tr));
	tr.allsolid = 1;
	tr.fraction = 1;
	memcpy (tr.endpos, end, sizeof (tr.endpos));
	tr.ent = -1;
	hlnc_rhc (&h, h.root, 0, 1, s, e, &tr);
	if (tr.fraction != 1)
		for (k = 0; k < 3; k++)
			tr.endpos[k] += o[k];
	return tr;
}

/*
 * Move query (spec 4.4) for a box over the world and the solid brush
 * entities. world_only = 0: tracelines and hull traces (every solid but
 * func_monsterclip); 1: the walk test's world-only moves (FL_WORLDBRUSH
 * solids only). The world first (all-solid ends it), then each candidate
 * whose bounds meet the move box (swept to the world's end point, +1 unit)
 * in entity order: it replaces the
 * result when all-solid, start-solid or nearer; a start-solid result stays
 * start-solid; a start-solid candidate that does not replace marks it.
 * ent: -2 world, -1 nothing hit, else the hlnc_bsp_t.solids index.
 */
static hlnc_trace_t hlnc_move (const hlnc_bsp_t *b, const float *start,
	const float *mins, const float *maxs, const float *end, int world_only)
{
	static const float	 zero[3] = { 0, 0, 0 };
	hlnc_trace_t		 tr, t;
	float			 lo[3], hi[3];
	int			 i, k;

	tr = hlnc_clip_model (b, 0, zero, start, mins, maxs, end);
	tr.ent = tr.fraction < 1 || tr.startsolid ? -2 : -1;
	if (tr.allsolid)
		return tr;
	for (k = 0; k < 3; k++)
	{
		lo[k] = (start[k] < tr.endpos[k] ? start[k] : tr.endpos[k])
			+ mins[k] - 1;
		hi[k] = (start[k] > tr.endpos[k] ? start[k] : tr.endpos[k])
			+ maxs[k] + 1;
	}
	for (i = 0; i < b->nsolids && !tr.allsolid; i++)
	{
		const hlnc_solid_t	*s = &b->solids[i];

		if (world_only ? !s->wbrush : s->mclip)
			continue;
		if (lo[0] > s->maxs[0] || lo[1] > s->maxs[1] || lo[2] > s->maxs[2]
			|| hi[0] < s->mins[0] || hi[1] < s->mins[1]
			|| hi[2] < s->mins[2])
			continue;
		t = hlnc_clip_model (b, s->model, s->org, start, mins, maxs, end);
		if (t.allsolid || t.startsolid || t.fraction < tr.fraction)
		{
			int	was = tr.startsolid;

			t.ent = i;
			tr = t;
			if (was)
				tr.startsolid = 1;
		}
		else if (t.startsolid)
			tr.startsolid = 1;
	}
	return tr;
}

/* world point contents with the CURRENT_* kinds reported as water (spec 1) */
static int hlnc_contents (const hlnc_bsp_t *b, const float *p)
{
	hlnc_hull_t	h;
	int		c;

	h.b = b;
	h.nodes = b->hull0;
	h.root = b->models[0].head[0];
	c = hlnc_pc (&h, h.root, p);
	if (c <= HLNC_CONTENTS_CURRENT_0 && c >= HLNC_CONTENTS_CURRENT_DOWN)
		c = HLNC_CONTENTS_WATER;
	return c;
}

/*
 * Ground check of a box at org (physics-movetypes.md 6 check_bottom): on
 * the ground when the world is solid just under all four bottom corners;
 * else a world-only line down from a step above the bottom centre (two
 * steps long) must hit, and the same line under each corner must hit no
 * more than a step below the centre's hit.
 */
static int hlnc_checkbottom (const hlnc_bsp_t *b, const float *org,
	const float *mins, const float *maxs)
{
	static const float	 zero[3] = { 0, 0, 0 };
	hlnc_hull_t		 h;
	hlnc_trace_t		 t;
	float			 lo[3], hi[3], a[3], z[3], centre;
	int			 c, k, flat = 1;

	for (k = 0; k < 3; k++)
	{
		lo[k] = org[k] + mins[k];
		hi[k] = org[k] + maxs[k];
	}
	h.b = b;
	h.nodes = b->hull0;
	h.root = b->models[0].head[0];
	a[2] = lo[2] - 1;
	for (c = 0; c < 4 && flat; c++)
	{
		a[0] = c & 1 ? hi[0] : lo[0];
		a[1] = c & 2 ? hi[1] : lo[1];
		flat = hlnc_pc (&h, h.root, a) == HLNC_CONTENTS_SOLID;
	}
	if (flat)
		return 1;

	a[0] = z[0] = (lo[0] + hi[0]) * 0.5f;
	a[1] = z[1] = (lo[1] + hi[1]) * 0.5f;
	a[2] = lo[2] + HLNC_STEPSIZE;
	z[2] = a[2] - 2 * HLNC_STEPSIZE;
	t = hlnc_move (b, a, zero, zero, z, 1);
	if (t.fraction == 1)
		return 0;
	centre = t.endpos[2];
	for (c = 0; c < 4; c++)
	{
		a[0] = z[0] = c & 1 ? hi[0] : lo[0];
		a[1] = z[1] = c & 2 ? hi[1] : lo[1];
		t = hlnc_move (b, a, zero, zero, z, 1);
		if (t.fraction == 1 || centre - t.endpos[2] > HLNC_STEPSIZE)
			return 0;
	}
	return 1;
}

/*
 * One world-only walk step of a box at *org by move (physics-movetypes.md
 * 6 walk_move, no enemy, no FL_PARTIALGROUND). Swimmers move straight (a
 * normal move: every solid but func_monsterclip) and may not leave the
 * water; an all-solid move has fraction 1 and passes, as in GoldSrc.
 * Walkers drop (world-only) from a step above the new position to a step
 * below it (retrying from the unraised position when the raised one is in
 * solid), must land and pass the ground check. On success *org is the new
 * position and 1 is returned.
 */
static int hlnc_movestep (const hlnc_bsp_t *b, float *org, const float *move,
	const float *mins, const float *maxs, int swim)
{
	hlnc_trace_t	t;
	float		to[3], end[3];
	int		k;

	for (k = 0; k < 3; k++)
		to[k] = org[k] + move[k];
	if (swim)
	{
		t = hlnc_move (b, org, mins, maxs, to, 0);
		if (t.fraction != 1
			|| hlnc_contents (b, t.endpos) == HLNC_CONTENTS_EMPTY)
			return 0;
		memcpy (org, t.endpos, sizeof (t.endpos));
		return 1;
	}
	to[2] += HLNC_STEPSIZE;
	memcpy (end, to, sizeof (end));
	end[2] -= 2 * HLNC_STEPSIZE;
	t = hlnc_move (b, to, mins, maxs, end, 1);
	if (t.allsolid)
		return 0;
	if (t.startsolid)
	{
		to[2] -= HLNC_STEPSIZE;
		t = hlnc_move (b, to, mins, maxs, end, 1);
		if (t.allsolid || t.startsolid)
			return 0;
	}
	if (t.fraction == 1 || !hlnc_checkbottom (b, t.endpos, mins, maxs))
		return 0;
	memcpy (org, t.endpos, sizeof (t.endpos));
	return 1;
}

/* yaw of (x, y) in whole degrees, [0, 360) (the engine's VecToYaw) */
static float hlnc_yaw (float x, float y)
{
	int	yaw;

	if (x == 0 && y == 0)
		return 0;
	yaw = (int)(atan2 (y, x) * 180 / 3.14159265358979323846);
	if (yaw < 0)
		yaw += 360;
	return (float)yaw;
}

/*---- entities */

typedef struct
{
	char	cls[48];
	float	org[3];
	float	ang[3], lip;
	int	model, sf, skin, has_org, rendermode, material;
} hlnc_ent_t;

/* next entity block of the lump text; 0 at the end */
static int hlnc_next_ent (const char **pp, const char *end, hlnc_ent_t *e)
{
	const char	*p = *pp;
	char		 key[64], val[256];
	int		 n;

	memset (e, 0, sizeof (*e));
	while (p < end && *p != '{')
		p++;
	if (p >= end)
		return 0;
	p++;
	for (;;)
	{
		const char	*q;

		while (p < end && *p != '"' && *p != '}')
			p++;
		if (p >= end || *p == '}')
			break;
		for (q = ++p; p < end && *p != '"'; p++)
			;
		n = (int)(p - q) < (int)sizeof (key) - 1 ? (int)(p - q)
			: (int)sizeof (key) - 1;
		memcpy (key, q, (size_t)n);
		key[n] = 0;
		if (p < end)
			p++;
		while (p < end && *p != '"' && *p != '}')
			p++;
		if (p >= end || *p == '}')
			break;
		for (q = ++p; p < end && *p != '"'; p++)
			;
		n = (int)(p - q) < (int)sizeof (val) - 1 ? (int)(p - q)
			: (int)sizeof (val) - 1;
		memcpy (val, q, (size_t)n);
		val[n] = 0;
		if (p < end)
			p++;
		if (!strcmp (key, "classname"))
		{
			n = n < (int)sizeof (e->cls) - 1 ? n : (int)sizeof (e->cls) - 1;
			memcpy (e->cls, val, (size_t)n);
			e->cls[n] = 0;
		}
		else if (!strcmp (key, "origin"))
		{
			e->has_org = sscanf (val, "%f %f %f", &e->org[0],
				&e->org[1], &e->org[2]) == 3;
			if (!e->has_org)
				e->org[0] = e->org[1] = e->org[2] = 0;
		}
		else if (!strcmp (key, "model") && val[0] == '*')
			e->model = atoi (val + 1);
		else if (!strcmp (key, "spawnflags"))
			e->sf = atoi (val);
		else if (!strcmp (key, "skin"))
			e->skin = atoi (val);
		else if (!strcmp (key, "rendermode"))
			e->rendermode = atoi (val);
		else if (!strcmp (key, "material"))
			e->material = atoi (val);
		else if (!strcmp (key, "angles"))
			sscanf (val, "%f %f %f", &e->ang[0], &e->ang[1], &e->ang[2]);
		else if (!strcmp (key, "angle"))
			e->ang[1] = (float)atof (val);	/* the engine's "angle" → "angles" */
		else if (!strcmp (key, "lip"))
			e->lip = (float)atof (val);
	}
	while (p < end && *p != '}')
		p++;
	*pp = p < end ? p + 1 : end;
	return 1;
}

/*---- BSP */

#define HLNC_LUMP(i)	(4 + 8 * (i))
#define HLNC_MAX_DEPTH	1024	/* deepest hull tree accepted (retail: 199) */

/*
 * Check a node array (children >= 0 are node indices) for cycles and for
 * paths deeper than HLNC_MAX_DEPTH, so every walk terminates and the trace
 * recursion is bounded. Compilers share clipnode subtrees, so a child may
 * precede its parent: an explicit-stack depth-first pass over every node.
 * Returns 0 when the trees are fine, else a reason.
 */
static const char *hlnc_tree_check (const hlnc_cnode_t *nodes, int n)
{
	int		*depth, *stack, i, sp, k;
	uint8_t		*state;		/* 0 new, 1 on the stack, 2 done */
	const char	*err = 0;

	if (!n)
		return 0;
	depth = calloc ((size_t)n, sizeof (*depth));
	stack = calloc ((size_t)n, sizeof (*stack));
	state = calloc ((size_t)n, 1);
	if (!depth || !stack || !state)
		err = "out of memory";
	for (i = 0; i < n && !err; i++)
	{
		if (state[i])
			continue;
		sp = 0;
		stack[sp++] = i;
		state[i] = 1;
		while (sp && !err)
		{
			int	top = stack[sp - 1], push = -1, d = 0;

			for (k = 0; k < 2; k++)
			{
				int	c = nodes[top].child[k];

				if (c < 0)
					continue;
				if (state[c] == 1)
					err = "hull tree has a cycle";
				else if (!state[c])
					push = c;
				else if (depth[c] > d)
					d = depth[c];
			}
			if (err)
				break;
			if (push >= 0)
			{
				stack[sp++] = push;	/* distinct nodes: sp <= n */
				state[push] = 1;
				continue;
			}
			depth[top] = d + 1;
			if (depth[top] > HLNC_MAX_DEPTH)
				err = "hull tree too deep";
			state[top] = 2;
			sp--;
		}
	}
	free (depth);
	free (stack);
	free (state);
	return err;
}

static const char *hlnc_load (const uint8_t *bsp, size_t len, hlnc_bsp_t *b)
{
	uint32_t	ofs[15], sz[15];
	int		i, k, nleafs;
	const char	*err;

	memset (b, 0, sizeof (*b));
	if (len < 4 + 8 * 15)
		return "truncated BSP header";
	if (hlnc_rd32 (bsp) != 30)
		return "not a BSP30 file (bad version)";
	for (i = 0; i < 15; i++)
	{
		ofs[i] = hlnc_rd32 (bsp + HLNC_LUMP (i));
		sz[i] = hlnc_rd32 (bsp + HLNC_LUMP (i) + 4);
		if (ofs[i] > len || sz[i] > len - ofs[i])
			return "lump outside the file";
	}
	/* planes (1), nodes (5), clipnodes (9), leafs (10), models (14) */
	if (sz[1] % 20 || sz[5] % 24 || sz[9] % 8 || sz[10] % 28
		|| sz[14] % 64)
		return "lump size not a whole number of records";
	b->nplanes = (int)(sz[1] / 20);
	b->nhull0 = (int)(sz[5] / 24);
	b->nclip = (int)(sz[9] / 8);
	b->nmodels = (int)(sz[14] / 64);
	nleafs = (int)(sz[10] / 28);
	if (!b->nplanes || !b->nhull0 || !b->nmodels || !nleafs)
		return "no collision";
	if (b->nhull0 > 32768 || b->nclip > 32768)
		return "too many nodes";
	b->planes = calloc ((size_t)b->nplanes, sizeof (*b->planes));
	b->hull0 = calloc ((size_t)b->nhull0, sizeof (*b->hull0));
	b->clip = calloc ((size_t)b->nclip + 1, sizeof (*b->clip));
	b->models = calloc ((size_t)b->nmodels, sizeof (*b->models));
	if (!b->planes || !b->hull0 || !b->clip || !b->models)
		return "out of memory";
	for (i = 0; i < b->nplanes; i++)
	{
		const uint8_t	*p = bsp + ofs[1] + 20 * (size_t)i;

		for (k = 0; k < 3; k++)
			b->planes[i].n[k] = hlnc_rdf (p + 4 * k);
		b->planes[i].d = hlnc_rdf (p + 12);
		b->planes[i].type = (int)hlnc_rd32 (p + 16);
		if (b->planes[i].type < 0 || b->planes[i].type > 5)
			b->planes[i].type = 3;
	}
	for (i = 0; i < b->nhull0; i++)
	{
		const uint8_t	*p = bsp + ofs[5] + 24 * (size_t)i;

		b->hull0[i].plane = (int)hlnc_rd32 (p);
		if (b->hull0[i].plane < 0 || b->hull0[i].plane >= b->nplanes)
			return "node plane out of range";
		for (k = 0; k < 2; k++)
		{
			int	c = hlnc_rd16 (p + 4 + 2 * k), leaf = -1 - c;

			if (c >= b->nhull0)
				return "node child out of range";
			if (c < 0)
			{
				/* leaf → its contents (hull 0 walks end on contents) */
				if (leaf >= nleafs)
					return "node leaf out of range";
				c = (int)hlnc_rd32 (bsp + ofs[10] + 28 * (size_t)leaf);
				if (c > HLNC_CONTENTS_EMPTY || c < -64)
					c = HLNC_CONTENTS_SOLID;
			}
			b->hull0[i].child[k] = c;
		}
	}
	for (i = 0; i < b->nclip; i++)
	{
		const uint8_t	*p = bsp + ofs[9] + 8 * (size_t)i;

		b->clip[i].plane = (int)hlnc_rd32 (p);
		if (b->clip[i].plane < 0 || b->clip[i].plane >= b->nplanes)
			return "clipnode plane out of range";
		for (k = 0; k < 2; k++)
		{
			int	c = hlnc_rd16 (p + 4 + 2 * k);

			if (c >= b->nclip)
				return "clipnode child out of range";
			b->clip[i].child[k] = c;
		}
	}
	if ((err = hlnc_tree_check (b->hull0, b->nhull0)) != NULL
		|| (err = hlnc_tree_check (b->clip, b->nclip)) != NULL)
		return err;
	for (i = 0; i < b->nmodels; i++)
	{
		const uint8_t	*p = bsp + ofs[14] + 64 * (size_t)i;

		for (k = 0; k < 3; k++)
		{
			b->models[i].mins[k] = hlnc_rdf (p + 4 * k);
			b->models[i].maxs[k] = hlnc_rdf (p + 12 + 4 * k);
		}
		for (k = 0; k < 4; k++)
		{
			/* a negative head is a contents value; a clip head one
			 * past the last clipnode (compilers write it for models
			 * without clip hulls) is an empty hull */
			b->models[i].head[k] = (int)hlnc_rd32 (p + 36 + 4 * k);
			if (k && b->models[i].head[k] == b->nclip)
				b->models[i].head[k] = HLNC_CONTENTS_EMPTY;
			if (b->models[i].head[k] >= (k ? b->nclip : b->nhull0))
				return "model head node out of range";
		}
	}
	return 0;
}

static void hlnc_free (hlnc_bsp_t *b)
{
	free (b->planes);
	free (b->hull0);
	free (b->clip);
	free (b->models);
	free (b->solids);
}


/* per-class solidity, door positions and the graph build (HLSDK) */
#include "../hlsdk/hl_nodegraph.h"

#if defined (HL_ENTCOOK_H) && !defined (HLE_FORMAT_ONLY)
/*
 * A GoldSrc map's entity chunk from its whole BSP30 file: the HLE1 blob of
 * the entity lump, then (16-aligned, when the map has a graph) its HLN1
 * node graph. The runtime finds the graph at align16(HLE header size).
 */
static const char *hle_cook_bsp (const uint8_t *bsp, size_t len,
	int big_endian, uint8_t **out, int *outlen, hle_stats_t *st,
	hln_stats_t *nst)
{
	uint8_t		*hle = NULL, *graph = NULL, *all;
	int		 hlen = 0, glen = 0, at;
	uint32_t	 eofs, elen;
	const char	*err;

	*out = NULL;
	*outlen = 0;
	if (len < 4 + 8 * 15)
		return "truncated BSP header";
	if (hlnc_rd32 (bsp) != 30)
		return "not a BSP30 file (bad version)";
	eofs = hlnc_rd32 (bsp + HLNC_LUMP (0));
	elen = hlnc_rd32 (bsp + HLNC_LUMP (0) + 4);
	if (eofs > len || elen > len - eofs)
		return "lump outside the file";
	if (!elen || elen > 0x7FFFFFFF)
		return "bad entity lump size";
	err = hle_build ((const char *)bsp + eofs, (int)elen, big_endian,
		&hle, &hlen, st);
	if (err)
		return err;
	err = hln_build (bsp, len, big_endian, &graph, &glen, nst);
	if (err)
	{
		free (hle);
		return err;
	}
	if (!graph)
	{
		*out = hle;
		*outlen = hlen;
		return 0;
	}
	at = (hlen + 15) & ~15;
	all = calloc (1, (size_t)at + (size_t)glen);
	if (!all)
	{
		free (hle);
		free (graph);
		return "out of memory";
	}
	memcpy (all, hle, (size_t)hlen);
	memcpy (all + at, graph, (size_t)glen);
	free (hle);
	free (graph);
	*out = all;
	*outlen = at + glen;
	return 0;
}
#endif /* HL_ENTCOOK_H cook */

#endif /* !HLN_FORMAT_ONLY */

#endif /* HL_NODECOOK_H */