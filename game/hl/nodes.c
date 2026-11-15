/***
*
*	Copyright (c) 1996-2002, Valve LLC. All rights reserved.
*	
*	This product contains software technology licensed from Id 
*	Software, Inc. ("Id Technology").  Id Technology (c) 1996 Id Software, Inc. 
*	All Rights Reserved.
*
*   Use, distribution, and modification of this source code and/or resulting
*   object code is restricted to non-commercial enhancements to products from
*   Valve LLC.  All other use, distribution, or modification is prohibited
*   without written permission from Valve LLC.
*
****/
/*
 * hl64 game/, Half Life 1 SDK licence (game/LICENSE-HLSDK).
 *
 * nodes.c -- the monsters' node graph queries (HLSDK dlls/nodes.cpp CGraph:
 * HullIndex, NodeType, FindNearestNode, FindShortestPath, HandleLinkEnt,
 * PathLength / NextNodeInRoute as used by FindCover / BuildNearestRoute)
 * over the cooked HLN1 graph of the monster's HLE slot (hle-format-v1.md
 * "Node graph", rec_node_graph). The graph is the HLSDK one link for link;
 * routes come from FindShortestPath's Dijkstra branch (the static routing
 * tables HLSDK derives from the same searches are not cooked), with the
 * path cut at MAX_PATH_SIZE as the table branch does. FindNearestNode is
 * HLSDK's "nearest node a line trace reaches" without the region index
 * (an acceleration): candidates are tried nearest first; in front of it
 * HLSDK's m_Cache (128 slots by CRC32 of the origin, the origin alone as
 * key, kept until the graph reloads: a door that moved since leaves a
 * stale answer, as in GoldSrc). HLN1 stores the dropped node origin
 * only, so it stands for m_vecOriginPeek too.
 */
#include <string.h>

#define HLN_FORMAT_ONLY
#include "tools/w64c/hl_nodecook.h"
#include "game/hl/monster.h"
#include "engine/res/res.h"	/* rtb_crc32: the CRC-32 of GoldSrc's CRC32_* */
#include "game/hl/savegame.h"

#define GRAPH_SLOTS		4	/* HLE slots */
#define CACHE_SIZE		128	/* nodes.h CACHE_SIZE */
#define SF_DOOR_USE_ONLY	256
#define SF_DOOR_NOMONSTERS	512
#define SF_DOOR_NO_AUTO_RETURN	32

typedef struct {
	const hln_header_t	*h;
	const hln_node_t	*nodes;
	const hln_link_t	*links;
	int			n;
	int			lastCoverSearch;
	/* CGraph::m_Cache: FindNearestNode's answers by origin (zeroed with
	 * the graph: an origin 0,0,0 not yet searched answers node 0, as in
	 * HLSDK) */
	struct { float v[3]; int16_t n; } cache[CACHE_SIZE];
	/* a saved game's lastCoverSearch and cache, waiting for the graph
	 * (h NULL): its first use keeps them instead of zeroing */
	int			kept;
} graph_t;

static graph_t	graphs[GRAPH_SLOTS];

/* Dijkstra scratch (MAX_NODES). FindShortestPath has its own distances
 * (dist_r): the cover searches keep their path lengths (HLSDK PathLength
 * over the static routing tables) in dist_a / dist_b across the routes
 * MoveToLocation / BuildRoute build inside their candidate loops. */
static float	dist_a[HLN_MAX_NODES], dist_b[HLN_MAX_NODES], dist_r[HLN_MAX_NODES];
static int16_t	prev[HLN_MAX_NODES];
static uint16_t	heap[HLN_MAX_NODES * 4];
static float	heapk[HLN_MAX_NODES * 4];

/* a saved cache naming a node this graph lacks (another cook of the
 * map): forgotten, as at a graph load */
static void graph_kept(graph_t *g, int slot)
{
	int i;

	for (i = 0; i < CACHE_SIZE; i++)
		if (g->cache[i].n >= g->n)
		{
			log_printf("nodes: slot %d: saved node cache names node %d of %d: cleared\n",
				slot, g->cache[i].n, g->n);
			g->lastCoverSearch = 0;
			memset(g->cache, 0, sizeof(g->cache));
			return;
		}
}

static graph_t *graph(int slot)
{
	graph_t		*g;
	uint32_t	size = 0;
	const void	*p;

	if (slot < 0 || slot >= GRAPH_SLOTS)
		return 0;
	g = &graphs[slot];
	if (!g->h)
	{
		p = rec_node_graph(slot, &size);
		if (!p)
			return 0;
		g->h = p;
		g->nodes = (const hln_node_t *)((const uint8_t *)p + g->h->ofs_nodes);
		g->links = (const hln_link_t *)((const uint8_t *)p + g->h->ofs_links);
		g->n = g->h->numnodes;
		if (g->kept)
			graph_kept(g, slot);
		else
		{
			g->lastCoverSearch = 0;
			memset(g->cache, 0, sizeof(g->cache));
		}
		g->kept = 0;
	}
	return g;
}

/* map open / close: forget the slot's graph pointers */
void Graph_SlotChanged(int slot)
{
	if (slot >= 0 && slot < GRAPH_SLOTS)
		memset(&graphs[slot], 0, sizeof(graphs[slot]));
}

/* Saved games: per slot whether its graph was read (its caches live:
 * stale answers persist as in GoldSrc), lastCoverSearch and the cache
 * entries that are not zero. A load keeps them for the slot's graph,
 * whose pointers its first use derives again. */
uint32_t Graph_SaveState(int op)
{
	static const uint8_t zero[sizeof(graphs[0].cache[0])];
	graph_t	*g;
	int	slot, i;
	uint32_t	k, nz;

	if (op == SG_SIZE)
		return sizeof(g->lastCoverSearch) + sizeof(g->cache[0]);
	for (slot = 0; slot < GRAPH_SLOTS; slot++)
	{
		g = &graphs[slot];
		if (op == SG_PUT)
		{
			if (!g->h && !g->kept)
			{
				sv_u8(0);
				continue;
			}
			sv_u8(1);
			sv_u32((uint32_t)g->lastCoverSearch);
			for (i = 0, nz = 0; i < CACHE_SIZE; i++)
				nz += memcmp(&g->cache[i], zero, sizeof(zero)) != 0;
			sv_u8(nz);
			for (i = 0; i < CACHE_SIZE; i++)
				if (memcmp(&g->cache[i], zero, sizeof(zero)))
				{
					sv_u8((uint32_t)i);
					sv_put(&g->cache[i], sizeof(g->cache[i]));
				}
			continue;
		}
		Graph_SlotChanged(slot);
		if (!sv_get_u8())
			continue;
		g->kept = 1;
		g->lastCoverSearch = (int)sv_get_u32();
		nz = sv_get_u8();
		for (k = 0; k < nz; k++)
		{
			i = (int)sv_get_u8();
			if (i >= CACHE_SIZE)
			{
				sv_fail("game: node cache entry out of range");
				return 0;
			}
			sv_get(&g->cache[i], sizeof(g->cache[i]));
			if (g->cache[i].n < NO_NODE || g->cache[i].n >= HLN_MAX_NODES)
			{
				sv_fail("game: node cache names no node");
				return 0;
			}
		}
		if (g->lastCoverSearch < 0 || g->lastCoverSearch > HLN_MAX_NODES)
			sv_fail("game: lastCoverSearch out of range");
	}
	return 0;
}

int Graph_Available(int slot) { return graph(slot) != 0; }
int Graph_NodeCount(int slot) { graph_t *g = graph(slot); return g ? g->n : 0; }

const float *Graph_NodeOrigin(int slot, int node)
{
	graph_t *g = graph(slot);
	return g && node >= 0 && node < g->n ? g->nodes[node].origin : g_vecZero;
}

int Graph_HullIndex(int e)
{
	const col_ent_t *c = ent_cv(e);

	if (c->movetype == MOVETYPE_FLY)
		return NODE_FLY_HULL;
	if (c->mins[0] == -12 && c->mins[1] == -12 && c->mins[2] == 0)
		return NODE_SMALL_HULL;
	if (c->mins[0] == -16 && c->mins[1] == -16 && c->mins[2] == 0)
		return NODE_HUMAN_HULL;	/* VEC_HUMAN_HULL_MIN */
	if (c->mins[0] == -32 && c->mins[1] == -32 && c->mins[2] == 0)
		return NODE_LARGE_HULL;
	return NODE_HUMAN_HULL;
}

int Graph_NodeType(int e)
{
	if (ent_cv(e)->movetype == MOVETYPE_FLY)
		return ent_phys(e)->waterlevel != 0 ? bits_NODE_WATER : bits_NODE_AIR;
	return bits_NODE_LAND;
}

static int hull_mask(int hull)
{
	switch (hull)
	{
	case NODE_SMALL_HULL: return HLN_LINK_SMALL;
	case NODE_LARGE_HULL: return HLN_LINK_LARGE;
	case NODE_FLY_HULL: return HLN_LINK_FLY;
	default: return HLN_LINK_HUMAN;
	}
}

/* the door / breakable entity of a link (its brush model in the slot) */
static int link_entity(int slot, const hln_link_t *l)
{
	static const cls_t door[3] = { CLS_FUNC_DOOR, CLS_FUNC_DOOR_ROTATING, CLS_FUNC_BREAKABLE };
	uint16_t	model = COL_MODEL(slot, l->lmodel);
	int		i, d;

	for (i = 0; i < 3; i++)
		for (d = ent_next_class(-1, door[i], slot); d >= 0; d = ent_next_class(d, door[i], slot))
			if (ent_cv(d)->model == model)
				return d;
	return -1;
}

/* HandleLinkEnt; static = NODEGRAPH_STATIC */
static int HandleLinkEnt(int slot, const hln_link_t *l, int afCapMask, int stat)
{
	int	kind = l->lent & HLN_LE_KIND, d, ts;

	if (kind == HLN_LE_NONE)
		return 1;
	if (kind == HLN_LE_DOOR)
	{
		d = link_entity(slot, l);
		if (d < 0)
			return 1;	/* dead path ent */
		ts = GetToggleState(d);
		if (l->lent & HLN_LE_USEONLY)
		{
			if (afCapMask & bits_CAP_OPEN_DOORS)
				return 1;
			return ts == TS_AT_TOP && (l->lent & HLN_LE_NOAUTORETURN);
		}
		if (ts == TS_AT_TOP && (l->lent & HLN_LE_NOAUTORETURN))
			return 1;
		if (afCapMask & bits_CAP_OPEN_DOORS)
		{
			if (!(l->lent & HLN_LE_NOMONSTERS) || stat)
				return 1;
		}
		return 0;
	}
	if (kind == HLN_LE_BREAKABLE)
		return stat;
	return 0;
}

int Graph_LinkPassable(int e, int slot, int src, int linkidx, int afCapMask)
{
	graph_t *g = graph(slot);

	(void)e;
	if (!g || src < 0 || src >= g->n || linkidx < 0 || linkidx >= g->nodes[src].num_links)
		return 0;
	return HandleLinkEnt(slot, &g->links[g->nodes[src].first_link + linkidx], afCapMask, 0);
}

/* FindNearestNode's candidate heap: (distance, node) pairs, nearest first,
 * the lower node on equal distances. It uses the Dijkstra queue's scratch:
 * neither search runs inside the other. nn_gen counts the fills of that
 * scratch (either search, ai_spread 2's scribble), so a search a spread
 * think paused in knows whether its heap is still there. */
#define nn_d	heapk
#define nn_i	heap
static int	nn_n;
static uint32_t	nn_gen;

static int nn_less(int a, int b)
{
	return nn_d[a] < nn_d[b] || (nn_d[a] == nn_d[b] && nn_i[a] < nn_i[b]);
}

static void nn_down(int i)
{
	int	c;
	float	td;
	uint16_t ti;

	for (;;)
	{
		c = 2 * i + 1;
		if (c >= nn_n)
			return;
		if (c + 1 < nn_n && nn_less(c + 1, c))
			c++;
		if (!nn_less(c, i))
			return;
		td = nn_d[i]; nn_d[i] = nn_d[c]; nn_d[c] = td;
		ti = nn_i[i]; nn_i[i] = nn_i[c]; nn_i[c] = ti;
		i = c;
	}
}

/* CGraph::FindNearestNode's cache slot: (CACHE_SIZE - 1) & CRC32 of the
 * origin's three floats as an x86 GoldSrc holds them (little-endian), so
 * the host and the big-endian cart pick HLSDK's slot */
static uint32_t cache_slot(const float org[3])
{
	uint8_t		b[12];
	uint32_t	u;
	int		i;

	for (i = 0; i < 3; i++)
	{
		memcpy(&u, &org[i], 4);
		b[4 * i] = (uint8_t)u;
		b[4 * i + 1] = (uint8_t)(u >> 8);
		b[4 * i + 2] = (uint8_t)(u >> 16);
		b[4 * i + 3] = (uint8_t)(u >> 24);
	}
	return rtb_crc32(0, b, sizeof(b)) & (CACHE_SIZE - 1);
}

/* the heap of the candidates after (d0, i0) in the search order (all of
 * them: d0 < 0) */
static void nn_fill(const graph_t *g, const float org[3], int afNodeTypes, float d0, int i0)
{
	int	k;
	float	d;

	nn_n = 0;
	nn_gen++;
	for (k = 0; k < g->n; k++)
	{
		if (!(g->nodes[k].info & afNodeTypes))
			continue;
		d = mv_dist(org, g->nodes[k].origin);
		if (!(d < 999999.0f) || d < d0 || (d == d0 && k <= i0))
			continue;
		nn_d[nn_n] = d;
		nn_i[nn_n] = (uint16_t)k;
		nn_n++;
	}
	for (k = nn_n / 2 - 1; k >= 0; k--)
		nn_down(k);
}

/* the search: candidates nearest first (ties: the lower node); the first
 * one a line trace reaches. A heap of every candidate's distance gives
 * the order of repeated nearest-untried scans in O(n log n). A spread
 * think of e may pause before each trace; another search having used the
 * heap meanwhile, it is rebuilt from the candidates after the one about
 * to be traced (the same order). */
static int nn_search(int e, const graph_t *g, const float org[3], int afNodeTypes)
{
	int		i;
	float		d;
	uint32_t	gen;
	col_trace_t	tr;

	nn_fill(g, org, afNodeTypes, -1.0f, -1);
	gen = nn_gen;
	while (nn_n > 0)
	{
		i = nn_i[0];
		d = nn_d[0];
		nn_n--;
		nn_d[0] = nn_d[nn_n];
		nn_i[0] = nn_i[nn_n];
		nn_down(0);
		if (Ai_PauseDue(e, 0))
		{
			Ai_Pause(e, 0);
			if (nn_gen != gen)
			{
				nn_fill(g, org, afNodeTypes, d, i);
				gen = nn_gen;
			}
		}
		mon_aiw.nnt++;
		UTIL_TraceLine(org, g->nodes[i].origin, ignore_monsters, -1, &tr);
		if (tr.frac == 1.0f)
			return i;
	}
	return NO_NODE;
}

int Graph_FindNearestNode(int e, const float org_in[3], int afNodeTypes)
{
	int		slot = ent_idc(e)->layer, best;
	graph_t		*g = graph(slot);
	uint32_t	hsh;
	uint64_t	t0;
	float		org[3];

	if (!g)
		return NO_NODE;
	if (afNodeTypes < 0)
		afNodeTypes = Graph_NodeType(e);
	/* HLSDK's cache: keyed by the origin alone (whatever node types the
	 * hit was searched for), never invalidated while the graph stays;
	 * the origin by value (it may be the monster's, moving while the
	 * search is paused) */
	VecCopy(org_in, org);
	hsh = cache_slot(org);
	if (VecEq(g->cache[hsh].v, org))
		return g->cache[hsh].n;
	t0 = Mon_PzBegin();
	mon_aiw.nn++;
	best = nn_search(e, g, org, afNodeTypes);
	VecCopy(org, g->cache[hsh].v);
	g->cache[hsh].n = (int16_t)best;
	Mon_PzEnd(MPZ_NN, t0);
	return best;
}

/* ---- Dijkstra (FindShortestPath, CQueuePriority) ----------------------------------- */

static int	hn;

static void heap_push(int node, float k)
{
	int i = hn++, p;

	while (i > 0)
	{
		p = (i - 1) / 2;
		if (heapk[p] <= k)
			break;
		heap[i] = heap[p];
		heapk[i] = heapk[p];
		i = p;
	}
	heap[i] = (uint16_t)node;
	heapk[i] = k;
}

static int heap_pop(float *k)
{
	int	top = heap[0], i = 0, c, last = heap[--hn];
	float	lk = heapk[hn];

	*k = heapk[0];
	for (;;)
	{
		c = 2 * i + 1;
		if (c >= hn)
			break;
		if (c + 1 < hn && heapk[c + 1] < heapk[c])
			c++;
		if (lk <= heapk[c])
			break;
		heap[i] = heap[c];
		heapk[i] = heapk[c];
		i = c;
	}
	heap[i] = (uint16_t)last;
	heapk[i] = lk;
	return top;
}

/* distances from start (-1 unreachable); stops at dest when dest >= 0 */
static void dijkstra(int slot, graph_t *g, int start, int dest, int iHull, int afCapMask, float *dist)
{
	int		mask = hull_mask(iHull), i, cur, v;
	float		cd, od;
	const hln_node_t *n;
	const hln_link_t *l;

	for (i = 0; i < g->n; i++)
		dist[i] = -1.0f;
	dist[start] = 0;
	prev[start] = (int16_t)start;
	hn = 0;
	nn_gen++;
	mon_aiw.dj++;
	heap_push(start, 0);
	while (hn > 0)
	{
		cur = heap_pop(&cd);
		if (cur == dest)
			break;
		if (cd > dist[cur] + 0.001f && dist[cur] >= 0)
			continue;
		mon_aiw.djpop++;
		n = &g->nodes[cur];
		for (i = 0; i < n->num_links; i++)
		{
			l = &g->links[n->first_link + i];
			v = l->dest;
			if (v >= g->n || (l->hulls & mask) != mask)
				continue;
			if ((l->lent & HLN_LE_KIND) && !HandleLinkEnt(slot, l, afCapMask, 1))
				continue;
			od = cd + mv_dist2d(n->origin, g->nodes[v].origin);	/* weight = 2D distance */
			if (dist[v] < -0.5f || od < dist[v] - 0.001f)
			{
				dist[v] = od;
				prev[v] = (int16_t)cur;
				if (hn < (int)(sizeof(heap) / sizeof(heap[0])))
					heap_push(v, od);
			}
		}
	}
}

int Graph_FindShortestPath(int slot, int *piPath, int iStart, int iDest, int iHull, int afCapMask)
{
	graph_t	*g = graph(slot);
	int	count, cur, i;

	if (!g || iStart < 0 || iStart >= g->n || iDest < 0 || iDest >= g->n)
		return 0;
	mon_stats_route();
	if (iStart == iDest)
	{
		piPath[0] = iStart;
		piPath[1] = iDest;
		return 2;
	}
	dijkstra(slot, g, iStart, iDest, iHull, afCapMask, dist_r);
	if (dist_r[iDest] < -0.5f)
		return 0;
	count = 1;
	for (cur = iDest; cur != iStart; cur = prev[cur])
		count++;
	/* the first MAX_PATH_SIZE nodes of the path start -> dest */
	cur = iDest;
	for (i = count - 1; i >= 0; i--)
	{
		if (i < MAX_PATH_SIZE)
			piPath[i] = cur;
		cur = prev[cur];
	}
	return count > MAX_PATH_SIZE ? MAX_PATH_SIZE : count;
}

/* the door to open on the route link from -> to (AdvanceRoute) */
int Graph_RouteDoor(int e, const float from[3], const float to[3])
{
	int		slot = ent_idc(e)->layer, src, dst, i;
	graph_t		*g = graph(slot);
	const hln_link_t *l;

	if (!g)
		return -1;
	src = Graph_FindNearestNode(e, from, -1);
	dst = Graph_FindNearestNode(e, to, -1);
	if (src < 0 || dst < 0)
		return -1;
	for (i = 0; i < g->nodes[src].num_links; i++)
	{
		l = &g->links[g->nodes[src].first_link + i];
		if (l->dest != dst)
			continue;
		if ((l->lent & HLN_LE_KIND) == HLN_LE_NONE)
			return -1;
		if (HandleLinkEnt(slot, l, MON(e)->m_afCapability, 0))
			return link_entity(slot, l);
		return -1;
	}
	return -1;
}

/* ---- cover and nearest routes (monsters.cpp FindCover / BuildNearestRoute) ---------- */

static int find_cover(int e, const float threat[3], const float viewofs[3], float mn, float mx,
	float out[3]);
static int nearest_route(int e, const float threat[3], const float viewofs[3], float mn, float mx);

/* Both searches are pure queries until the route they take: one
 * CheckLocalMove memo scope (monsters.c) each. The threat and view offset
 * are HLSDK by-value Vectors (a spread think's search may pause while the
 * entity they came from moves). */
int Graph_FindCover(int e, const float threat[3], const float viewofs[3], float mn, float mx,
	float out[3])
{
	float	t[3], v[3];
	int	ok;

	VecCopy(threat, t);
	VecCopy(viewofs, v);
	Mon_LocalMoveScope(1);
	ok = find_cover(e, t, v, mn, mx, out);
	Mon_LocalMoveScope(0);
	return ok;
}

int Graph_BuildNearestRoute(int e, const float threat[3], const float viewofs[3], float mn, float mx)
{
	float	t[3], v[3];
	int	ok;

	VecCopy(threat, t);
	VecCopy(viewofs, v);
	Mon_LocalMoveScope(1);
	ok = nearest_route(e, t, v, mn, mx);
	Mon_LocalMoveScope(0);
	return ok;
}

/* the path-length tables of a cover search: a spread think's own
 * (aithink.c: it may pause inside the search), else the shared ones */
typedef char cover_tables_fit[HL64_AI_SCRATCH_BYTES >= 2 * HLN_MAX_NODES * (int)sizeof(float) ? 1 : -1];

static void cover_tables(float **da, float **db)
{
	float *t = Ai_CoverTables();

	*da = t ? t : dist_a;
	*db = t ? t + HLN_MAX_NODES : dist_b;
}

static int find_cover(int e, const float threat[3], const float viewofs[3], float mn, float mx,
	float out[3])
{
	int		slot = ent_idc(e)->layer, my, th, hull, i, node;
	graph_t		*g = graph(slot);
	CBaseMonster	*m = MON(e);
	float		look[3], a[3], d, pm, pt, *da, *db;
	col_trace_t	tr;

	if (!g)
		return 0;
	cover_tables(&da, &db);
	my = Graph_FindNearestNode(e, ent_cv(e)->origin, -1);
	th = Graph_FindNearestNode(e, threat, -1);
	hull = Graph_HullIndex(e);
	if (my == NO_NODE)
		return 0;
	if (th == NO_NODE)
		th = my;
	VecAdd(threat, viewofs, look);
	dijkstra(slot, g, my, -1, hull, m->m_afCapability, da);
	if (th != my)
		dijkstra(slot, g, th, -1, hull, m->m_afCapability, db);
	for (i = 0; i < g->n; i++)
	{
		node = (i + g->lastCoverSearch) % g->n;
		g->lastCoverSearch = node + 1;
		d = mv_dist(ent_cv(e)->origin, g->nodes[node].origin);
		if (d >= mn && d < mx)
		{
			Ai_PausePoint(e);
			VecAdd(g->nodes[node].origin, viewofs, a);
			mon_aiw.cover++;
			UTIL_TraceLineGlass(a, look, ignore_monsters, ignore_glass, e, &tr);
			if (tr.frac != 1.0f)
			{
				/* PathLength: 0 when there is no route */
				pm = da[node] < 0 ? 0 : da[node];
				pt = th == my ? pm : (db[node] < 0 ? 0 : db[node]);
				if (my == th || pm <= pt)
				{
					if (Mon_FValidateCover(e, g->nodes[node].origin) &&
						Mon_MoveToLocation(e, ACT_RUN, 0, g->nodes[node].origin))
					{
						if (out)
							VecCopy(g->nodes[node].origin, out);
						return 1;
					}
				}
			}
		}
	}
	return 0;
}

static int nearest_route(int e, const float threat[3], const float viewofs[3], float mn, float mx)
{
	int		slot = ent_idc(e)->layer, my, hull, i, node;
	graph_t		*g = graph(slot);
	CBaseMonster	*m = MON(e);
	float		look[3], a[3], d, *da, *db;
	col_trace_t	tr;

	if (!g)
		return 0;
	cover_tables(&da, &db);
	my = Graph_FindNearestNode(e, ent_cv(e)->origin, -1);
	hull = Graph_HullIndex(e);
	if (my == NO_NODE)
		return 0;
	VecAdd(threat, viewofs, look);
	/* NextNodeInRoute(my, node, hull, 0) != my: reachable without doors */
	dijkstra(slot, g, my, -1, hull, 0, da);
	for (i = 0; i < g->n; i++)
	{
		node = (i + g->lastCoverSearch) % g->n;
		g->lastCoverSearch = node + 1;
		if (node == my || da[node] < 0)
			continue;
		d = mv_dist(threat, g->nodes[node].origin);
		if (d > mn && d < mx)
		{
			Ai_PausePoint(e);
			VecAdd(g->nodes[node].origin, m->view_ofs, a);
			mon_aiw.cover++;
			UTIL_TraceLine(a, look, ignore_monsters, e, &tr);
			if (tr.frac == 1.0f && Mon_BuildRoute(e, g->nodes[node].origin, bits_MF_TO_LOCATION, -1))
			{
				VecCopy(g->nodes[node].origin, m->m_vecMoveGoal);
				return 1;
			}
		}
	}
	return 0;
}

/* ai_spread 2 (aithink.c): what other thinks' searches would leave in the
 * shared scratch */
void Graph_ScribbleScratch(void)
{
	int i;

	for (i = 0; i < HLN_MAX_NODES; i++)
	{
		dist_a[i] = dist_b[i] = dist_r[i] = (float)(i * 7 % 13) - 1.5f;
		prev[i] = (int16_t)((i * 31) % HLN_MAX_NODES);
	}
	for (i = 0; i < HLN_MAX_NODES * 4; i++)
	{
		heap[i] = (uint16_t)i;
		heapk[i] = -1.0f;
	}
	hn = nn_n = 0;
	nn_gen++;
}
