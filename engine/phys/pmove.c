/* SPDX-License-Identifier: MIT */
/*
 * pmove.c -- the player entity and the engine side of player movement
 * (docs/specs/pmove-host.md; docs/architecture.md 3.2 step 5, 6.2, 8).
 *
 * Each frame: build the move record (physents from the open layers and
 * the spatial grid, ladders as move-ents, the command), run the game's
 * pm_shared adapter (game_pm_move), write the player's collision view
 * back, link it with trigger touches and run the impact touches of the
 * pmove touch list. The services pm_shared calls back into (contents,
 * player traces, position tests) live here over engine/coll.
 *
 * Not before M3: conveyor ground entities (their speed / movedir belong
 * to the M3 components), the game's player prethink / think / postthink
 * around the move, and audio for pm_shared's footstep sounds (counted).
 */
#include <math.h>
#include <string.h>

#include "hl64/game.h"
#include "hl64/grid.h"
#include "engine/core/dbg.h"
#include "engine/core/log.h"
#include "engine/core/sys.h"
#include "engine/in/in.h"
#include "engine/phys/player.h"
#include "engine/world/wld.h"

phy_pm_t phy_pm;

const float phy_pm_mins[4][3] = {
	{ -16, -16, -36 }, { -16, -16, -18 }, { 0, 0, 0 }, { -32, -32, -32 } };
const float phy_pm_maxs[4][3] = {
	{ 16, 16, 36 }, { 16, 16, 18 }, { 0, 0, 0 }, { 32, 32, 32 } };

#define VIEW_HEIGHT	28.0f	/* HLSDK VEC_VIEW */
static struct {
	ent_player_t		pl;
	col_ent_t		*e;		/* entity slot 1 */
	int			map;		/* home map, WLD_NO_MAP */
	uint8_t			active;
	uint8_t			printed;	/* a pm_shared message this frame */
	phy_player_prof_t	prof;
} P = { .map = WLD_NO_MAP };

/* ---- services (pmove-host.md 3) ------------------------------------------ */

void phy_pm_print(const char *msg)
{
	if (P.printed)
		return;
	P.printed = 1;
	log_printf("pmove: %s%s", msg, msg[0] && msg[strlen(msg) - 1] == '\n' ? "" : "\n");
}

void phy_pm_sound(int channel, const char *sample, float volume, float attenuation,
	int flags, int pitch)
{
	(void)channel;
	(void)sample;
	(void)volume;
	(void)attenuation;
	(void)flags;
	(void)pitch;
	P.prof.sounds++;
}

static int box_has(const col_ent_t *e, const float p[3])
{
	int k;

	for (k = 0; k < 3; k++)
		if (p[k] < e->absmin[k] || p[k] > e->absmax[k])
			return 0;
	return 1;
}

int phy_pm_true_contents(const float p[3])
{
	return col_true_contents(p);
}

int phy_pm_contents(const float p[3], int *truecont)
{
	int w = col_true_contents(p), i;

	if (truecont)
		*truecont = w;
	if (w <= COL_CURRENT_0 && w >= COL_CURRENT_DOWN)
		w = COL_WATER;
	for (i = 0; i < phy_pm.npe; i++)
	{
		const col_ent_t *e = &phy_pm.pe[i];

		if (e->index == 0 || e->skin >= -1 || e->model == COL_NONE || !box_has(e, p))
			continue;
		if (col_model_contents(e->model, e->origin, e->angles, 0, p) == COL_SOLID)
			return e->skin;
	}
	return w;
}

int phy_pm_model_bounds(uint16_t model, float mins[3], float maxs[3])
{
	const wld_layer_t *L = wld_layer(COL_MODEL_SLOT(model));
	int k;

	if (!L || !col_model_bounds(model, mins, maxs))
		return 0;
	for (k = 0; k < 3; k++)
	{
		mins[k] += L->gofs[k];
		maxs[k] += L->gofs[k];
	}
	return 1;
}

void phy_pm_trace_ent(const col_ent_t *pe, int usehull, const float start[3], const float end[3],
	col_trace_t *tr)
{
	col_clip(pe, start, phy_pm_mins[usehull], phy_pm_maxs[usehull], end, tr);
}

/* the physents a player trace with these flags examines */
static int pe_skip(const col_ent_t *e, int i, int flags, int ignore_pe)
{
	if (i == ignore_pe || e->solid == COL_SOLID_NOT || e->solid == COL_SOLID_TRIGGER)
		return 1;
	if ((flags & PHY_PM_WORLD_ONLY) && e->index != 0)
		return 1;
	if ((flags & PHY_PM_GLASS_IGNORE) && e->rendermode != COL_RENDER_NORMAL)
		return 1;
	if ((flags & PHY_PM_STUDIO_IGNORE) && e->studio != COL_NONE)
		return 1;
	return 0;
}

void phy_pm_trace(const float start[3], const float end[3], int flags, int usehull, int ignore_pe,
	col_trace_t *tr)
{
	int i, k;

	memset(tr, 0, sizeof(*tr));
	tr->frac = 1;
	tr->ent = -1;
	for (k = 0; k < 3; k++)
		tr->end[k] = end[k];
	for (i = 0; i < phy_pm.npe; i++)
	{
		col_trace_t t;

		if (pe_skip(&phy_pm.pe[i], i, flags, ignore_pe))
			continue;
		phy_pm_trace_ent(&phy_pm.pe[i], usehull, start, end, &t);
		/* trace-and-hulls.md 4.4 step 5 */
		if (t.allsolid || t.startsolid || t.frac < tr->frac)
		{
			int ss = tr->startsolid;

			*tr = t;
			tr->ent = (int16_t)i;
			if (ss)
				tr->startsolid = 1;
		}
		else if (t.startsolid)
			tr->startsolid = 1;
	}
	/* the pmove rule (spec 3): all solid => start solid; start solid =>
	 * fraction 0 and the end point the start [probe:measured T-segments
	 * pmove traces] */
	if (tr->allsolid)
		tr->startsolid = 1;
	if (tr->startsolid)
	{
		tr->frac = 0;
		for (k = 0; k < 3; k++)
			tr->end[k] = start[k];
	}
}

int phy_pm_test(const float pos[3], int usehull, col_trace_t *tr)
{
	int i;

	for (i = 0; i < phy_pm.npe; i++)
	{
		col_trace_t t;

		if (pe_skip(&phy_pm.pe[i], i, 0, -1))
			continue;
		phy_pm_trace_ent(&phy_pm.pe[i], usehull, pos, pos, &t);
		if (t.startsolid || t.allsolid)
		{
			if (tr)
			{
				*tr = t;
				tr->ent = (int16_t)i;
			}
			return i;
		}
	}
	if (tr)
	{
		memset(tr, 0, sizeof(*tr));
		tr->frac = 1;
		tr->ent = -1;
		memcpy(tr->end, pos, sizeof(tr->end));
	}
	return -1;
}

/* ---- physents (pmove-host.md 2) ------------------------------------------- */

static void world_physent(col_ent_t *o, const wld_layer_t *L)
{
	memset(o, 0, sizeof(*o));
	memcpy(o->origin, L->gofs, sizeof(o->origin));
	memcpy(o->mins, L->models[0].mins, sizeof(o->mins));
	memcpy(o->maxs, L->models[0].maxs, sizeof(o->maxs));
	for (int k = 0; k < 3; k++)
	{
		o->absmin[k] = L->gofs[k] + o->mins[k];
		o->absmax[k] = L->gofs[k] + o->maxs[k];
	}
	o->index = 0;
	o->owner = COL_NONE;
	o->model = COL_MODEL(L->slot, 0);
	o->studio = COL_NONE;
	o->skin = 0;
	o->solid = COL_SOLID_BSP;
	o->rendermode = COL_RENDER_NORMAL;
}

static int boxes_meet(const float amin[3], const float amax[3], const float bmin[3],
	const float bmax[3])
{
	int k;

	for (k = 0; k < 3; k++)
		if (amin[k] > bmax[k] || amax[k] < bmin[k])
			return 0;
	return 1;
}

/* squared distance from p to an entity's linked box */
static float box_dist2(const col_ent_t *e, const float p[3])
{
	float d2 = 0;
	int k;

	for (k = 0; k < 3; k++)
	{
		float d = p[k] < e->absmin[k] ? e->absmin[k] - p[k]
			: p[k] > e->absmax[k] ? p[k] - e->absmax[k] : 0;
		d2 += d * d;
	}
	return d2;
}

/* Keep the `room` entries of idx[0..n) nearest to p (ascending index kept). */
static int keep_nearest(uint16_t *idx, int n, int room, const float p[3])
{
	float d[2 * HL64_PM_PHYSENTS];
	int i, j, kept = 0;

	for (i = 0; i < n; i++)
		d[i] = box_dist2(ent_get(idx[i]), p);
	/* threshold: the room-th smallest distance, by selection */
	for (i = 0; i < n && kept < room; i++)
	{
		int better = 0;

		for (j = 0; j < n; j++)
			if (d[j] < d[i] || (d[j] == d[i] && j < i))
				better++;
		if (better < room)
			idx[kept++] = idx[i];
	}
	return kept;
}

static void build_physents(uint32_t dt_us)
{
	ent_player_t *pl = &P.pl;
	float mins[3], maxs[3], dt = (float)dt_us * 1e-6f;
	uint16_t idx[2 * HL64_PM_PHYSENTS], lad[2 * HL64_PM_MOVEENTS];
	int n, nl = 0, i, k, over = 0;
	const wld_layer_t *home = NULL;

	for (k = 0; k < 3; k++)
	{
		float r = fabsf(pl->velocity[k] + pl->basevelocity[k]) * dt + 18 + 1;

		mins[k] = pl->origin[k] + pl->mins[k] - r;
		maxs[k] = pl->origin[k] + pl->maxs[k] + r;
	}
	phy_pm.npe = 0;
	phy_pm.nme = 0;
	/* physent 0: the home layer; then the other open layers it meets */
	for (i = 0; i < HL64_WORLD_LAYERS; i++)
	{
		const wld_layer_t *L = wld_layer(i);

		if (L && L->map == P.map)
			home = L;
	}
	if (home)
		world_physent(&phy_pm.pe[phy_pm.npe++], home);
	for (i = 0; i < HL64_WORLD_LAYERS; i++)
	{
		const wld_layer_t *L = wld_layer(i);
		col_ent_t w;

		if (!L || L == home)
			continue;
		world_physent(&w, L);
		if (boxes_meet(w.absmin, w.absmax, mins, maxs))
			phy_pm.pe[phy_pm.npe++] = w;
	}

	/* entities: solids and contents brushes, ascending index */
	n = ent_query(mins, maxs, ENT_Q_SOLID | ENT_Q_CONTENTS, idx, 2 * HL64_PM_PHYSENTS);
	for (i = k = 0; i < n; i++)
	{
		const col_ent_t *e = ent_get(idx[i]);

		/* func_monsterclip blocks monsters only: never a player physent
		 * [probe:measured PM-bot] */
		if (!e || e->index == ENT_PLAYER || e->owner == ENT_PLAYER
			|| (e->flags & COL_FL_MONSTERCLIP))
			continue;
		if (e->solid == COL_SOLID_NOT && e->skin == COL_LADDER)
		{
			if (nl < 2 * HL64_PM_MOVEENTS)
				lad[nl++] = idx[i];
			continue;
		}
		idx[k++] = idx[i];
	}
	n = k;
	if (n > HL64_PM_PHYSENTS - phy_pm.npe)
	{
		n = keep_nearest(idx, n, HL64_PM_PHYSENTS - phy_pm.npe, pl->origin);
		over = 1;
	}
	for (i = 0; i < n; i++)
		phy_pm.pe[phy_pm.npe++] = *ent_get(idx[i]);
	if (nl > HL64_PM_MOVEENTS)
	{
		nl = keep_nearest(lad, nl, HL64_PM_MOVEENTS, pl->origin);
		over = 1;
	}
	for (i = 0; i < nl; i++)
		phy_pm.me[phy_pm.nme++] = *ent_get(lad[i]);
	if (over)
	{
		if (!P.printed)
			log_printf("pmove: physents over the cap: kept the %d / %d nearest\n",
				HL64_PM_PHYSENTS, HL64_PM_MOVEENTS);
		P.printed = 1;
		P.prof.overflow++;
	}
	if ((uint32_t)phy_pm.npe > P.prof.physents_max)
		P.prof.physents_max = (uint32_t)phy_pm.npe;
}

/* ---- the player entity ----------------------------------------------------- */

static void sync_view(void)
{
	memcpy(P.e->origin, P.pl.origin, sizeof(P.e->origin));
	memcpy(P.e->mins, P.pl.mins, sizeof(P.e->mins));
	memcpy(P.e->maxs, P.pl.maxs, sizeof(P.e->maxs));
	P.e->angles[0] = 0;
	P.e->angles[1] = P.pl.view_angles[1];
	P.e->angles[2] = 0;
	P.e->solid = COL_SOLID_SLIDEBOX;
}

int phy_player_active(void)
{
	return P.active;
}

int phy_player_map(void)
{
	return P.map;
}

ent_player_t *phy_player(void)
{
	return &P.pl;
}

static int cmd_setpos(const dbg_cmd_t *c);
static int cmd_look(const dbg_cmd_t *c);

int phy_player_spawn(int map, const float org[3], const float ang[3])
{
	const wld_layer_t *L;
	int k;

	if (map < 0 || map >= wld_map_count() || !(L = wld_open(map)))
		return -1;
	if (!P.active)
	{
		P.e = ent_get(ENT_PLAYER);
		if (!P.e)
			P.e = ent_alloc_at(ENT_PLAYER);
		if (!P.e)
			return -1;
		/* HLSDK CBasePlayer::Spawn state */
		memset(&P.pl, 0, sizeof(P.pl));
		P.pl.view_ofs[2] = VIEW_HEIGHT;
		P.pl.flags = PL_FL_CLIENT;
		P.pl.movetype = PL_MOVE_WALK;
		P.pl.friction = 1;
		P.pl.gravity = 1;
		P.pl.groundentity = -1;
		P.pl.watertype = COL_EMPTY;
		memcpy(P.pl.mins, phy_pm_mins[0], sizeof(P.pl.mins));
		memcpy(P.pl.maxs, phy_pm_maxs[0], sizeof(P.pl.maxs));
		P.active = 1;
		dbg_handler_set(DBG_SETPOS, cmd_setpos);
		dbg_handler_set(DBG_LOOK, cmd_look);
	}
	P.map = map;
	for (k = 0; k < 3; k++)
	{
		P.pl.origin[k] = org[k] + L->gofs[k];
		P.pl.velocity[k] = 0;
		P.pl.basevelocity[k] = 0;
		P.pl.view_angles[k] = ang[k];
		P.pl.angles[k] = ang[k];
	}
	P.pl.fall_velocity = 0;
	P.pl.flags &= ~PL_FL_ONGROUND;
	P.pl.groundentity = -1;
	in_set_angles(ang);
	sync_view();
	ent_link(P.e);
	log_printf("player: %s %.9g %.9g %.9g ang %.9g %.9g\n", wld_map_name(map),
		(double)org[0], (double)org[1], (double)org[2], (double)ang[0], (double)ang[1]);
	return 0;
}

int phy_player_spawn_start(int map)
{
	const wld_layer_t *L;
	float org[3], ang[3] = { 0, 0, 0 };
	int k;

	if (map < 0 || map >= wld_map_count() || !(L = wld_open(map)))
		return -1;
	for (k = 0; k < 3; k++)
	{
		org[k] = L->has_start ? L->start_org[k]
			: 0.5f * (L->models[0].mins[k] + L->models[0].maxs[k]);
		if (L->has_start)
			ang[k] = L->start_ang[k];
	}
	org[2] += 1;
	return phy_player_spawn(map, org, ang);
}

void phy_player_frame(const in_cmd_t *cmd, uint64_t now_us)
{
	ent_player_t *pl = &P.pl;
	uint64_t t0;
	uint32_t us;
	int i, k;
	uint16_t done[HL64_PM_TOUCH];
	int ndone = 0;

	if (!P.active)
		return;
	t0 = sys_time_us();
	P.printed = 0;

	/* collision residency around the player (architecture.md 5.3) */
	{
		float mins[3], maxs[3];

		for (k = 0; k < 3; k++)
		{
			mins[k] = pl->origin[k] + pl->mins[k];
			maxs[k] = pl->origin[k] + pl->maxs[k];
		}
		col_want(mins, maxs);
	}

	/* step 2: the player's think before the move (game) */
	game_player_prethink(pl);

	/* steps 4-5: physents and the move */
	phy_pm.pl = pl;
	phy_pm.cmd = *cmd;
	phy_pm.time_ms = (float)(now_us / 1000u);
	phy_pm.ntouch = 0;
	build_physents((uint32_t)cmd->msec * 1000u);
	game_pm_move(&phy_pm);

	/* step 6: the collision view; base velocity is set again by this
	 * frame's touches (trigger_push) */
	sync_view();
	for (k = 0; k < 3; k++)
		pl->basevelocity[k] = 0;

	/* step 7: link with trigger touches, then the impact touches of the
	 * touch list, each entity once */
	phy_link_touch(P.e);
	for (i = 0; i < phy_pm.ntouch; i++)
	{
		int pe = phy_pm.touch[i].ent, j;
		col_ent_t *other;
		uint16_t idx;

		if (pe < 0 || pe >= phy_pm.npe)
			continue;
		idx = phy_pm.pe[pe].index;
		for (j = 0; j < ndone && done[j] != idx; j++)
			;
		if (j < ndone)
			continue;
		done[ndone++] = idx;
		other = ent_get(idx);
		if (other && ent_get(ENT_PLAYER) == P.e)
			phy_impact(P.e, other);
	}

	us = (uint32_t)(sys_time_us() - t0);
	P.prof.frames++;
	P.prof.us_sum += us;
	if (us > P.prof.us_max)
		P.prof.us_max = us;
}

void phy_player_prof(phy_player_prof_t *p, int reset)
{
	*p = P.prof;
	if (reset)
		memset(&P.prof, 0, sizeof(P.prof));
}

/* ---- debug commands (architecture.md 12) ------------------------------------ */

/* setpos map x y z pitch yaw roll: the player's origin, map-local */
static int cmd_setpos(const dbg_cmd_t *c)
{
	float org[3], ang[3];
	int k;

	for (k = 0; k < 3; k++)
	{
		org[k] = c->arg[1 + k].f;
		ang[k] = c->arg[4 + k].f;
	}
	return phy_player_spawn((int)c->arg[0].u, org, ang);
}

static int cmd_look(const dbg_cmd_t *c)
{
	float ang[3] = { c->arg[0].f, c->arg[1].f, 0 };

	in_set_angles(ang);
	return 0;
}

static int cmd_noclip(const dbg_cmd_t *c)
{
	if (!P.active)
		return -1;
	P.pl.movetype = c->arg[0].u ? PL_MOVE_NOCLIP : PL_MOVE_WALK;
	log_printf("player: noclip %s\n", c->arg[0].u ? "on" : "off");
	return 0;
}

static int cmd_god(const dbg_cmd_t *c)
{
	if (!P.active)
		return -1;
	if (c->arg[0].u)
		P.pl.flags |= PL_FL_GODMODE;
	else
		P.pl.flags &= ~PL_FL_GODMODE;
	log_printf("player: god %s\n", c->arg[0].u ? "on" : "off");
	return 0;
}

void phy_player_init(void)
{
	game_pm_init();
	dbg_handler_set(DBG_NOCLIP, cmd_noclip);
	dbg_handler_set(DBG_GOD, cmd_god);
}
