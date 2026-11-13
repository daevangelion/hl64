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
 * hl64 cook tools, Half Life 1 SDK licence: tools/hlsdk/LICENSE-HLSDK.
 *
 * hl_nodegraph.h — the HLSDK half of the node graph cook
 * (tools/w64c/hl_nodecook.h includes it after its BSP reader and engine
 * services): which brush entities are solid at spawn and how (per-class
 * Spawn solidity, CFuncWall FL_WORLDBRUSH, func_breakable glass), the
 * door link-entity flags and a START_OPEN func_door's spawn position
 * (doors.cpp CBaseDoor::Spawn, subs.cpp SetMovedir), and hln_build:
 * nodes.cpp CTestHull::BuildNodeGraph (LinkVisibleNodes, the per-hull walk
 * test, RejectInlineLinks) run offline on the map's own hulls.
 */
#ifndef HL_NODEGRAPH_H
#define HL_NODEGRAPH_H

#define HLNC_MAX_COORD	65536.0f	/* node origins beyond this: hostile input */

/* SOLID_BSP at spawn (HLSDK Spawn of each brush class); world excluded */
static int hlnc_is_solid (const hlnc_ent_t *e)
{
	const char	*c = e->cls;

	if (e->model <= 0)
		return 0;
	if (!strcmp (c, "func_door") || !strcmp (c, "func_door_rotating")
		|| !strcmp (c, "momentary_door"))
		return !(e->sf & 8) && !e->skin;	/* SF_DOOR_PASSABLE, func_water contents */
	if (!strcmp (c, "func_water"))
		return 0;
	if (!strcmp (c, "func_rotating"))
		return !(e->sf & 64);			/* SF_BRUSH_ROTATE_NOT_SOLID */
	if (!strcmp (c, "func_rot_button"))
		return !(e->sf & 1);			/* SF_ROTBUTTON_NOTSOLID */
	if (!strcmp (c, "func_wall_toggle"))
		return !(e->sf & 1);			/* SF_WALL_START_OFF */
	if (!strcmp (c, "func_conveyor"))
		return !(e->sf & 2);			/* SF_CONVEYOR_NOTSOLID */
	if (!strcmp (c, "func_train") || !strcmp (c, "func_tracktrain")
		|| !strcmp (c, "func_pendulum"))
		return !(e->sf & 8);			/* *_PASSABLE */
	if (!strcmp (c, "func_illusionary") || !strcmp (c, "func_ladder")
		|| !strcmp (c, "func_friction") || !strcmp (c, "func_mortar_field"))
		return 0;			/* SOLID_NOT */
	if (!strncmp (c, "func_", 5) || !strcmp (c, "button_target")
		|| !strcmp (c, "momentary_rot_button"))
		return 1;
	return 0;		/* trigger_*, env_*, game_* brush volumes */
}

/* FL_WORLDBRUSH (CFuncWall::Spawn and its subclasses; CBreakable: unbreakable
 * glass not rendered normally) */
static int hlnc_wbrush (const hlnc_ent_t *e)
{
	const char	*c = e->cls;

	if (!strcmp (c, "func_breakable"))
		return e->material == 7 && e->rendermode != 0;	/* matUnbreakableGlass */
	return !strcmp (c, "func_wall") || !strcmp (c, "func_wall_toggle")
		|| !strcmp (c, "func_conveyor") || !strcmp (c, "func_monsterclip");
}

static int hlnc_lent (const hlnc_ent_t *e)
{
	if (!strcmp (e->cls, "func_door") || !strcmp (e->cls, "func_door_rotating"))
		return HLN_LE_DOOR
			| ((e->sf & 256) ? HLN_LE_USEONLY : 0)
			| ((e->sf & 32) ? HLN_LE_NOAUTORETURN : 0)
			| ((e->sf & 512) ? HLN_LE_NOMONSTERS : 0);
	if (!strcmp (e->cls, "func_breakable"))
		return HLN_LE_BREAKABLE;
	return HLN_LE_NONE;
}

/* where a linear func_door with SF_DOOR_START_OPEN spawns: m_vecPosition2
 * (SetMovedir; CBaseDoor::Spawn) relative to its origin key */
static void hlnc_door_open (const hlnc_ent_t *e, const hlnc_model_t *m,
	float *org)
{
	float	dir[3], d, pr, yr;
	int	k;

	if (e->ang[0] == 0 && e->ang[1] == -1 && e->ang[2] == 0)
		dir[0] = 0, dir[1] = 0, dir[2] = 1;
	else if (e->ang[0] == 0 && e->ang[1] == -2 && e->ang[2] == 0)
		dir[0] = 0, dir[1] = 0, dir[2] = -1;
	else
	{
		pr = e->ang[0] * 3.14159265358979323846f / 180;
		yr = e->ang[1] * 3.14159265358979323846f / 180;
		dir[0] = cosf (pr) * cosf (yr);
		dir[1] = cosf (pr) * sinf (yr);
		dir[2] = -sinf (pr);
	}
	d = -e->lip;
	for (k = 0; k < 3; k++)
		d += fabsf (dir[k] * (m->maxs[k] - m->mins[k] - 2));
	for (k = 0; k < 3; k++)
		org[k] += dir[k] * d;
}

typedef struct
{
	float	org[3];
	int	info, first, num;
} hlnc_node_t;

typedef struct
{
	int	src, dest, hulls, lent, lmodel;
	float	weight;
} hlnc_link_t;

/*
 * The node graph of a BSP30 map. *out = NULL (and 0 returned) when the map
 * has no nodes or HLSDK's LinkVisibleNodes would fail (a node seeing 128 or
 * more others: the game then runs without a graph too).
 */
static const char *hln_build (const uint8_t *bsp, size_t len, int big_endian,
	uint8_t **out, int *outlen, hln_stats_t *stats)
{
	static const float hmins[3][3] = { { -12, -12, 0 }, { -16, -16, 0 },
		{ -32, -32, 0 } };
	static const float hmaxs[3][3] = { { 12, 12, 24 }, { 16, 16, 72 },
		{ 32, 32, 64 } };
	static const float lmins[3] = { -32, -32, -32 }, lmaxs[3] = { 32, 32, 32 };
	static const float zero[3] = { 0, 0, 0 };
	hlnc_bsp_t	 b;
	hlnc_node_t	*nodes = NULL;
	hlnc_link_t	*pool = NULL;
	hlnc_ent_t	 e;
	hln_stats_t	 st;
	const char	*err, *p, *end;
	uint32_t	 eofs, elen;
	int		 nnodes = 0, cap = 0, npool = 0, i, j, k, hull;
	uint8_t		*blob;
	size_t		 size;

	*out = NULL;
	*outlen = 0;
	memset (&st, 0, sizeof (st));
	if (stats)
		*stats = st;
	if ((err = hlnc_load (bsp, len, &b)) != NULL)
	{
		hlnc_free (&b);
		return err;
	}
	eofs = hlnc_rd32 (bsp + HLNC_LUMP (0));
	elen = hlnc_rd32 (bsp + HLNC_LUMP (0) + 4);
	p = (const char *)bsp + eofs;
	end = p + elen;
	/* nodes (CNodeEnt::Spawn order) and solid brush entities */
	while (hlnc_next_ent (&p, end, &e))
	{
		if (!strcmp (e.cls, "info_node") || !strcmp (e.cls, "info_node_air"))
		{
			if (nnodes >= HLN_MAX_NODES)
				continue;	/* "cNodes >= MAX_NODES" */
			if (nnodes == cap)
			{
				hlnc_node_t *n2;

				cap = cap ? cap * 2 : 64;
				n2 = realloc (nodes, (size_t)cap * sizeof (*nodes));
				if (!n2)
				{
					err = "out of memory";
					goto done;
				}
				nodes = n2;
			}
			for (k = 0; k < 3; k++)
				if (!(fabsf (e.org[k]) <= HLNC_MAX_COORD))
				{
					err = "node origin out of range";
					goto done;
				}
			memset (&nodes[nnodes], 0, sizeof (nodes[nnodes]));
			memcpy (nodes[nnodes].org, e.org, sizeof (e.org));
			nodes[nnodes].info = e.cls[9] ? HLN_NODE_AIR : 0;
			nnodes++;
		}
		else if (hlnc_is_solid (&e) && e.model < b.nmodels)
		{
			hlnc_solid_t	*s2 = realloc (b.solids,
				(size_t)(b.nsolids + 1) * sizeof (*b.solids));

			if (!s2)
			{
				err = "out of memory";
				goto done;
			}
			b.solids = s2;
			s2 = &b.solids[b.nsolids++];
			s2->model = e.model;
			memcpy (s2->org, e.org, sizeof (e.org));
			if ((e.sf & 1) && !strcmp (e.cls, "func_door"))
				hlnc_door_open (&e, &b.models[e.model], s2->org);
			for (k = 0; k < 3; k++)
			{
				s2->mins[k] = b.models[e.model].mins[k] + s2->org[k];
				s2->maxs[k] = b.models[e.model].maxs[k] + s2->org[k];
			}
			s2->lent = hlnc_lent (&e);
			s2->wbrush = hlnc_wbrush (&e);
			s2->mclip = !strcmp (e.cls, "func_monsterclip");
		}
	}
	st.nodes = nnodes;
	if (!nnodes)
		goto done;

	/* water nodes; land nodes to the floor + NODE_HEIGHT */
	for (i = 0; i < nnodes; i++)
	{
		hlnc_trace_t	t;
		float		down[3];

		if (nodes[i].info & HLN_NODE_AIR)
			continue;
		if (hlnc_contents (&b, nodes[i].org) == HLNC_CONTENTS_WATER)
		{
			nodes[i].info |= HLN_NODE_WATER;
			continue;
		}
		nodes[i].info |= HLN_NODE_LAND;
		memcpy (down, nodes[i].org, sizeof (down));
		down[2] -= 384;
		t = hlnc_move (&b, nodes[i].org, zero, zero, down, 0);
		nodes[i].org[2] = t.endpos[2] + HLNC_NODE_HEIGHT;
	}

	/* LinkVisibleNodes */
	pool = malloc ((size_t)nnodes * HLNC_MAX_INITIAL * sizeof (*pool));
	if (!pool)
	{
		err = "out of memory";
		goto done;
	}
	for (i = 0; i < nnodes; i++)
	{
		nodes[i].first = npool;
		nodes[i].num = 0;
		for (j = 0; j < nnodes; j++)
		{
			hlnc_trace_t	t, r;
			int		lent = -1;

			if (j == i || (nodes[i].info & 7) != (nodes[j].info & 7))
				continue;
			t = hlnc_move (&b, nodes[i].org, zero, zero, nodes[j].org, 0);
			if (t.startsolid)
				continue;
			if (t.fraction != 1)
			{
				r = hlnc_move (&b, nodes[j].org, zero, zero,
					nodes[i].org, 0);
				if (r.fraction == 1 || r.ent != t.ent || t.ent < 0)
					continue;
				lent = t.ent;
			}
			memset (&pool[npool], 0, sizeof (pool[npool]));
			pool[npool].src = i;
			pool[npool].dest = j;
			if (lent >= 0)
			{
				pool[npool].lent = b.solids[lent].lent;
				pool[npool].lmodel = b.solids[lent].model;
				if (!(pool[npool].lent & HLN_LE_KIND))
					pool[npool].lent = -1;	/* unhandled ent */
			}
			npool++;
			if (++nodes[i].num == HLNC_MAX_INITIAL)
			{
				/* LinkVisibleNodes fails: the game has no graph */
				npool = 0;
				nnodes = 0;
				st.nodes = 0;
				goto done;
			}
		}
	}
	st.visible = npool;

	/* walk rejection (links stay in their node's slice of the pool) */
	for (i = 0; i < nnodes; i++)
	{
		hlnc_node_t	*src = &nodes[i];

		for (j = 0; j < src->num; j++)
		{
			hlnc_link_t	*l = &pool[src->first + j];
			hlnc_node_t	*dst = &nodes[l->dest];
			int		 skip = 0;

			l->hulls = HLN_LINK_SMALL | HLN_LINK_HUMAN | HLN_LINK_LARGE
				| HLN_LINK_FLY;
			for (hull = 0; hull < 4; hull++)
			{
				if (skip && hull < 3)
					continue;
				if (hull < 3)
				{
					float	org[3], yaw, dist, move[3], ss, d;
					int	step, failed = 0;

					memcpy (org, src->org, sizeof (org));
					yaw = hlnc_yaw (dst->org[0] - org[0],
						dst->org[1] - org[1]);
					dist = sqrtf ((dst->org[0] - org[0])
						* (dst->org[0] - org[0])
						+ (dst->org[1] - org[1])
						* (dst->org[1] - org[1]));
					for (step = 0; step < dist && !failed;
						step += HLNC_HULL_STEP)
					{
						ss = HLNC_HULL_STEP;
						if (step + ss >= dist - 1)
							ss = dist - step - 1;
						d = yaw * 3.14159265358979323846f * 2 / 360;
						move[0] = cosf (d) * ss;
						move[1] = sinf (d) * ss;
						move[2] = 0;
						if (!hlnc_movestep (&b, org, move,
							hmins[hull], hmaxs[hull],
							(src->info & HLN_NODE_WATER) != 0))
							failed = 1;
					}
					if (!failed)
					{
						float	dx = org[0] - dst->org[0],
							dy = org[1] - dst->org[1],
							dz = org[2] - dst->org[2];

						if (dx * dx + dy * dy + dz * dz > 64 * 64)
							failed = 1;
					}
					if (failed)
					{
						if (hull == 0)
							l->hulls &= ~(HLN_LINK_SMALL
								| HLN_LINK_HUMAN | HLN_LINK_LARGE);
						else if (hull == 1)
							l->hulls &= ~(HLN_LINK_HUMAN
								| HLN_LINK_LARGE);
						else
							l->hulls &= ~HLN_LINK_LARGE;
						if (hull < 2)
							skip = 1;
					}
				}
				else
				{
					float		a[3], c[3];
					hlnc_trace_t	t;

					memcpy (a, src->org, sizeof (a));
					memcpy (c, dst->org, sizeof (c));
					a[2] += 32;
					c[2] += 32;
					t = hlnc_move (&b, a, lmins, lmaxs, c, 0);
					if (t.startsolid || t.fraction < 1)
						l->hulls &= ~HLN_LINK_FLY;
				}
			}
			if (!l->hulls)
			{
				*l = pool[src->first + src->num - 1];
				src->num--;
				j--;
				st.rejected_walk++;
			}
		}
	}

	/* RejectInlineLinks */
	for (i = 0; i < nnodes; i++)
	{
		hlnc_node_t	*src = &nodes[i];

		for (j = 0; j < src->num; j++)
		{
			hlnc_link_t	*l = &pool[src->first + j];
			float		 cx = nodes[l->dest].org[0] - src->org[0];
			float		 cy = nodes[l->dest].org[1] - src->org[1];
			float		 cd = sqrtf (cx * cx + cy * cy);

			l->weight = cd;
			if (cd != 0)
			{
				cx /= cd;
				cy /= cd;
			}
			for (k = 0; k < src->num; k++)
			{
				const hlnc_link_t *m = &pool[src->first + k];
				float	tx, ty, td;

				if (k == j)
					continue;
				tx = nodes[m->dest].org[0] - src->org[0];
				ty = nodes[m->dest].org[1] - src->org[1];
				td = sqrtf (tx * tx + ty * ty);
				if (td != 0)
				{
					tx /= td;
					ty /= td;
				}
				if (cx * tx + cy * ty >= 0.998f && td < cd)
				{
					*l = pool[src->first + src->num - 1];
					src->num--;
					j--;
					st.rejected_inline++;
					break;
				}
			}
		}
	}

	/* drop links through entities HandleLinkEnt never passes; compact */
	{
		int	n = 0;

		for (i = 0; i < nnodes; i++)
		{
			int	first = nodes[i].first, num = nodes[i].num;

			nodes[i].first = n;
			nodes[i].num = 0;
			for (j = 0; j < num; j++)
			{
				if (pool[first + j].lent < 0)
					continue;
				pool[n++] = pool[first + j];
				nodes[i].num++;
			}
		}
		npool = n;
	}
	st.links = npool;
	if (npool > 0xFFFF)
	{
		err = "too many links";
		goto done;
	}

	/* land nodes back to the floor; serialize */
	size = sizeof (hln_header_t) + (size_t)nnodes * sizeof (hln_node_t)
		+ (size_t)npool * sizeof (hln_link_t);
	size = (size + 3) & ~(size_t)3;
	blob = calloc (1, size);
	if (!blob)
	{
		err = "out of memory";
		goto done;
	}
	{
		hln_header_t	*h = (hln_header_t *)blob;
		hln_node_t	*on = (hln_node_t *)(blob + sizeof (*h));
		hln_link_t	*ol = (hln_link_t *)(on + nnodes);

		h->magic = HLN_MAGIC;
		h->version = HLN_VERSION;
		h->flags = (uint16_t)(hln_host_big () ? HLN_F_BIGENDIAN : 0);
		h->size = (uint32_t)size;
		h->numnodes = (uint16_t)nnodes;
		h->numlinks = (uint16_t)npool;
		h->ofs_nodes = sizeof (*h);
		h->ofs_links = (uint32_t)((uint8_t *)ol - blob);
		for (i = 0; i < nnodes; i++)
		{
			memcpy (on[i].origin, nodes[i].org, sizeof (on[i].origin));
			if (nodes[i].info & HLN_NODE_LAND)
				on[i].origin[2] -= HLNC_NODE_HEIGHT;
			on[i].first_link = (uint16_t)nodes[i].first;
			on[i].num_links = (uint8_t)nodes[i].num;
			on[i].info = (uint8_t)nodes[i].info;
		}
		for (i = 0; i < npool; i++)
		{
			ol[i].dest = (uint16_t)pool[i].dest;
			ol[i].hulls = (uint8_t)pool[i].hulls;
			ol[i].lent = (uint8_t)pool[i].lent;
			ol[i].lmodel = (uint16_t)pool[i].lmodel;
		}
		if (!big_endian != !hln_host_big ())
		{
			hln_swap_body (blob, h);
			h->flags = big_endian ? HLN_F_BIGENDIAN : 0;
			hln_swap_header (h);
		}
	}
	*out = blob;
	*outlen = (int)size;
done:
	if (stats)
		*stats = st;
	free (pool);
	free (nodes);
	hlnc_free (&b);
	return err;
}

#endif /* HL_NODEGRAPH_H */
