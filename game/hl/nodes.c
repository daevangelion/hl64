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
 * (an acceleration): candidates are tried nearest first. HLN1 stores the
 * dropped node origin only, so it stands for m_vecOriginPeek too.
 */
#include <string.h>

#define HLN_FORMAT_ONLY
#include "tools/w64c/hl_nodecook.h"
#include "game/hl/monster.h"

#define GRAPH_SLOTS		4	/* HLE slots */
#define NEAREST_CACHE		64
#define SF_DOOR_USE_ONLY	256
#define SF_DOOR_NOMONSTERS	512
#define SF_DOOR_NO_AUTO_RETURN	32

typedef struct {
	const hln_header_t	*h;
	const hln_node_t	*nodes;
	const hln_link_t	*links;
	int			n;
	int			lastCoverSearch;
	struct { float v[3]; int16_t n; uint8_t valid; } cache[NEAREST_CACHE];
} graph_t;

static graph_t	graphs[GRAPH_SLOTS];

/* Dijkstra scratch (MAX_NODES) */
static float	dist_a[HLN_MAX_NODES], dist_b[HLN_MAX_NODES];
static int16_t	prev[HLN_MAX_NODES];
static uint16_t	heap[HLN_MAX_NODES * 4];
static float	heapk[HLN_MAX_NODES * 4];

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
		g->lastCoverSearch = 0;
		memset(g->cache, 0, sizeof(g->cache));
	}
	return g;
}

/* map open / close: forget the slot's graph pointers */
void Graph_SlotChanged(int slot)
{
	if (slot >= 0 && slot < GRAPH_SLOTS)
		memset(&graphs[slot], 0, sizeof(graphs[slot]));
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
 * neither search runs inside the other. */
#define nn_d	heapk
#define nn_i	heap
static int	nn_n;

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

int Graph_FindNearestNode(int e, const float org[3], int afNodeTypes)
{
	int		slot = ent_idc(e)->layer, i, best, k;
	graph_t		*g = graph(slot);
	uint32_t	hsh;
	float		d;
	col_trace_t	tr;

	if (!g)
		return NO_NODE;
	if (afNodeTypes < 0)
		afNodeTypes = Graph_NodeType(e);
	{
		uint32_t b[3];

		memcpy(b, org, sizeof(b));
		hsh = (b[0] * 2654435761u ^ b[1] * 40503u ^ b[2] * 2246822519u ^ (uint32_t)afNodeTypes) &
			(NEAREST_CACHE - 1);
	}
	if (g->cache[hsh].valid && VecEq(g->cache[hsh].v, org))
		return g->cache[hsh].n;
	mon_aiw.nn++;
	best = NO_NODE;
	/* candidates nearest first (ties: the lower node); the first one a
	 * line trace reaches. A heap of every candidate's distance gives the
	 * order of repeated nearest-untried scans in O(n log n). */
	nn_n = 0;
	for (k = 0; k < g->n; k++)
	{
		if (!(g->nodes[k].info & afNodeTypes))
			continue;
		d = mv_dist(org, g->nodes[k].origin);
		if (!(d < 999999.0f))
			continue;
		nn_d[nn_n] = d;
		nn_i[nn_n] = (uint16_t)k;
		nn_n++;
	}
	for (k = nn_n / 2 - 1; k >= 0; k--)
		nn_down(k);
	while (nn_n > 0)
	{
		i = nn_i[0];
		nn_n--;
		nn_d[0] = nn_d[nn_n];
		nn_i[0] = nn_i[nn_n];
		nn_down(0);
		mon_aiw.nnt++;
		UTIL_TraceLine(org, g->nodes[i].origin, ignore_monsters, -1, &tr);
		if (tr.frac == 1.0f)
		{
			best = i;
			break;
		}
	}
	VecCopy(org, g->cache[hsh].v);
	g->cache[hsh].n = (int16_t)best;
	g->cache[hsh].valid = 1;
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
	int	count, cur, i, all[HLN_MAX_NODES];

	if (!g || iStart < 0 || iStart >= g->n || iDest < 0 || iDest >= g->n)
		return 0;
	mon_stats_route();
	if (iStart == iDest)
	{
		piPath[0] = iStart;
		piPath[1] = iDest;
		return 2;
	}
	dijkstra(slot, g, iStart, iDest, iHull, afCapMask, dist_a);
	if (dist_a[iDest] < -0.5f)
		return 0;
	count = 1;
	for (cur = iDest; cur != iStart; cur = prev[cur])
		count++;
	cur = iDest;
	for (i = count - 1; i >= 0; i--)
	{
		all[i] = cur;
		cur = prev[cur];
	}
	if (count > MAX_PATH_SIZE)
		count = MAX_PATH_SIZE;
	memcpy(piPath, all, (size_t)count * sizeof(int));
	return count;
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
 * CheckLocalMove memo scope (monsters.c) each. */
int Graph_FindCover(int e, const float threat[3], const float viewofs[3], float mn, float mx,
	float out[3])
{
	int ok;

	Mon_LocalMoveScope(1);
	ok = find_cover(e, threat, viewofs, mn, mx, out);
	Mon_LocalMoveScope(0);
	return ok;
}

int Graph_BuildNearestRoute(int e, const float threat[3], const float viewofs[3], float mn, float mx)
{
	int ok;

	Mon_LocalMoveScope(1);
	ok = nearest_route(e, threat, viewofs, mn, mx);
	Mon_LocalMoveScope(0);
	return ok;
}

static int find_cover(int e, const float threat[3], const float viewofs[3], float mn, float mx,
	float out[3])
{
	int		slot = ent_idc(e)->layer, my, th, hull, i, node;
	graph_t		*g = graph(slot);
	CBaseMonster	*m = MON(e);
	float		look[3], a[3], d, pm, pt;
	col_trace_t	tr;

	if (!g)
		return 0;
	my = Graph_FindNearestNode(e, ent_cv(e)->origin, -1);
	th = Graph_FindNearestNode(e, threat, -1);
	hull = Graph_HullIndex(e);
	if (my == NO_NODE)
		return 0;
	if (th == NO_NODE)
		th = my;
	VecAdd(threat, viewofs, look);
	dijkstra(slot, g, my, -1, hull, m->m_afCapability, dist_a);
	if (th != my)
		dijkstra(slot, g, th, -1, hull, m->m_afCapability, dist_b);
	for (i = 0; i < g->n; i++)
	{
		node = (i + g->lastCoverSearch) % g->n;
		g->lastCoverSearch = node + 1;
		d = mv_dist(ent_cv(e)->origin, g->nodes[node].origin);
		if (d >= mn && d < mx)
		{
			VecAdd(g->nodes[node].origin, viewofs, a);
			mon_aiw.cover++;
			UTIL_TraceLineGlass(a, look, ignore_monsters, ignore_glass, e, &tr);
			if (tr.frac != 1.0f)
			{
				/* PathLength: 0 when there is no route */
				pm = dist_a[node] < 0 ? 0 : dist_a[node];
				pt = th == my ? pm : (dist_b[node] < 0 ? 0 : dist_b[node]);
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
	float		look[3], a[3], d;
	col_trace_t	tr;

	if (!g)
		return 0;
	my = Graph_FindNearestNode(e, ent_cv(e)->origin, -1);
	hull = Graph_HullIndex(e);
	if (my == NO_NODE)
		return 0;
	VecAdd(threat, viewofs, look);
	/* NextNodeInRoute(my, node, hull, 0) != my: reachable without doors */
	dijkstra(slot, g, my, -1, hull, 0, dist_a);
	for (i = 0; i < g->n; i++)
	{
		node = (i + g->lastCoverSearch) % g->n;
		g->lastCoverSearch = node + 1;
		if (node == my || dist_a[node] < 0)
			continue;
		d = mv_dist(threat, g->nodes[node].origin);
		if (d > mn && d < mx)
		{
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
