/* SPDX-License-Identifier: MIT */
/*
 * pm.c -- host harness for docs/specs/pmove-host.md: the real pmove host
 * (engine/phys/pmove.c), HLSDK pm_shared through its adapter (game/pm),
 * the spatial grid and engine/coll, driven by a line script on stdin.
 * test/m2/pmbot.py builds the scripts from the GoldSrc probe fixtures
 * (PM-bot runs: PM2, PM9; T-segments pmove traces: PM3) and compares the
 * output; `selftest` runs the self-contained tests PM1, PM3 (rules),
 * PM4, PM5, PM6 and PM7 and prints "selftest ok" or FAIL lines.
 *
 *   hl64-pm --root CONTENT < script
 *
 * Positions in the script and the output are map-local: the player block
 * and the physents are home-local (pmove-host.md 2.1); the harness adds
 * the map's placement g_M only where it places entities. Script lines:
 *   map NAME
 *   ent SLOT SUB ORG ANG MINS MAXS SOLID SKIN RENDERMODE OWNER FLAGS
 *        (re)place entity slot SLOT (>= 2): SUB = submodel ("*N") or -1
 *        for a box; OWNER = slot or -1; linked without touch
 *   free SLOT
 *   reset T ORG VEL BASEVEL FL VIEWOFS WL WT GE INDUCK DUCKTIME FALLVEL
 *         PUNCH MOVETYPE HULL          the player state (GE = slot or -1)
 *   cmd T PITCH YAW ROLL FM SM UM BUTTONS MSEC
 *        one move at game time T (s); prints
 *        "f ORG VEL FL VIEWOFS WL WT GE INDUCK DUCKTIME FALLVEL PUNCH
 *         MOVETYPE HULL OLDBUTTONS NPE NME NTOUCH TOUCHSLOTS..."
 *        then "p SLOT..." (the physents' entity slots, in order)
 *   pmtrace USEHULL START END       PM_PlayerTrace(PM_WORLD_ONLY) with the
 *        world as the only physent; prints
 *        "pm AS SS IO IW FRAC END NORMAL DIST PE"
 *   selftest ORG [ORG...]           the synthetic tests at the first
 *                                   candidate (player origin, standing)
 *                                   with the room they need (spot_ok)
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hl64/game.h"
#include "hl64/grid.h"
#include "hl64/limits.h"
#include "hl64/settings.h"
#include "engine/core/dbg.h"
#include "engine/core/sys.h"
#include "engine/in/in.h"
#include "engine/io/io.h"
#include "engine/mem/carve.h"
#include "engine/phys/player.h"
#include "engine/world/wld.h"
#include "plat/host/host.h"

static int map = -1;
static const float *gofs;
static int fails;

#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

static int rd(char **p, float *v, int n)
{
	int i;

	for (i = 0; i < n; i++)
	{
		char *e;

		v[i] = strtof(*p, &e);
		if (e == *p)
			return -1;
		*p = e;
	}
	return 0;
}

static int rdi(char **p, int *v)
{
	char *e;

	*v = (int)strtol(*p, &e, 10);
	if (e == *p)
		return -1;
	*p = e;
	return 0;
}

/* ---- entities ---------------------------------------------------------------- */

static col_ent_t *place(int slot, int sub, const float org[3], const float ang[3],
	const float mins[3], const float maxs[3], int solid, int skin, int rmode, int owner,
	uint32_t flags)
{
	col_ent_t *e = ent_get(slot);
	int k;

	if (!e)
		e = ent_alloc_at(slot);
	if (!e)
		return NULL;
	ent_unlink(e);
	for (k = 0; k < 3; k++)
	{
		e->origin[k] = org[k] + gofs[k];
		e->angles[k] = ang[k];
		e->mins[k] = mins[k];
		e->maxs[k] = maxs[k];
	}
	e->model = sub >= 0 ? COL_MODEL(wld_open(map)->slot, sub) : COL_NONE;
	e->solid = (uint8_t)solid;
	e->skin = (int16_t)skin;
	e->rendermode = (uint8_t)rmode;
	e->owner = owner >= 0 ? (uint16_t)owner : COL_NONE;
	e->flags = flags;
	ent_link(e);
	return e;
}

/* ---- the player ------------------------------------------------------------------ */

static void reset_player(const float *v, int ge)
{
	/* v: t, org[3], vel[3], bv[3], fl, vo[3], wl, wt, ge(skip), ind, dt,
	 * fv, pa[3], mt, hull */
	ent_player_t *pl;
	float ang[3] = { 0, 0, 0 };
	int k, hull = (int)v[23];

	phy_player_spawn(map, v + 1, ang);
	pl = phy_player();
	for (k = 0; k < 3; k++)
	{
		pl->velocity[k] = v[4 + k];
		pl->basevelocity[k] = v[7 + k];
		pl->view_ofs[k] = v[11 + k];
		pl->punchangle[k] = v[19 + k];
		pl->mins[k] = phy_pm_mins[hull][k];
		pl->maxs[k] = phy_pm_maxs[hull][k];
	}
	pl->flags = (uint32_t)v[10];
	pl->waterlevel = (int8_t)v[14];
	pl->watertype = (int8_t)v[15];
	pl->groundentity = (int16_t)ge;
	pl->in_duck = (uint8_t)v[16];
	pl->duck_time = v[17];
	pl->fall_velocity = v[18];
	pl->movetype = (uint8_t)v[22];
	pl->usehull = (uint8_t)hull;
}

static void print_player(void)
{
	const ent_player_t *pl = phy_player();
	int i;

	printf("f %.9g %.9g %.9g %.9g %.9g %.9g %lu %.9g %d %d %d %d %.9g %.9g %.9g %.9g %.9g %d %d "
		"%u %d %d %d", (double)pl->origin[0], (double)pl->origin[1],
		(double)pl->origin[2], (double)pl->velocity[0], (double)pl->velocity[1],
		(double)pl->velocity[2], (unsigned long)pl->flags, (double)pl->view_ofs[2],
		pl->waterlevel, pl->watertype, pl->groundentity, pl->in_duck, (double)pl->duck_time,
		(double)pl->fall_velocity, (double)pl->punchangle[0], (double)pl->punchangle[1],
		(double)pl->punchangle[2], pl->movetype, pl->usehull, pl->oldbuttons, phy_pm.npe,
		phy_pm.nme, phy_pm.ntouch);
	for (i = 0; i < phy_pm.ntouch; i++)
		printf(" %d", phy_pm.touch[i].ent >= 0 ? phy_pm.pe[phy_pm.touch[i].ent].index : -1);
	printf("\np");
	for (i = 0; i < phy_pm.npe; i++)
		printf(" %d", phy_pm.pe[i].index);
	printf("\n");
}

static void move(float t, const float ang[3], float fm, float sm, float um, int buttons, int msec)
{
	in_cmd_t c;

	memset(&c, 0, sizeof(c));
	memcpy(c.viewangles, ang, sizeof(c.viewangles));
	c.forwardmove = fm;
	c.sidemove = sm;
	c.upmove = um;
	c.buttons = (uint16_t)buttons;
	c.msec = (uint8_t)msec;
	phy_player_frame(&c, (uint64_t)((double)t * 1e6 + 0.5));
}

/* world-only physent list of the open map (the home world at the origin) */
static void world_only(void)
{
	const wld_layer_t *L = wld_open(map);
	col_ent_t *o = &phy_pm.pe[0];

	memset(o, 0, sizeof(*o));
	o->owner = COL_NONE;
	o->studio = COL_NONE;
	o->model = COL_MODEL(L->slot, 0);
	o->solid = COL_SOLID_BSP;
	phy_pm.npe = 1;
	phy_pm.nme = 0;
}

static void pmtrace(int usehull, const float s[3], const float e[3])
{
	col_trace_t t;

	world_only();
	phy_pm_trace(s, e, PHY_PM_WORLD_ONLY, usehull, -1, &t);
	printf("pm %d %d %d %d %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %d\n", t.allsolid,
		t.startsolid, t.inopen, t.inwater, (double)t.frac, (double)t.end[0],
		(double)t.end[1], (double)t.end[2], (double)t.normal[0],
		(double)t.normal[1], (double)t.normal[2], (double)t.dist, t.ent);
}

/* ---- self-contained tests --------------------------------------------------------- */

static uint32_t touch_log[256];
static int ntouch_log;

static void on_touch(uint16_t fn, col_ent_t *self, col_ent_t *other)
{
	(void)fn;
	if (ntouch_log < 256)
		touch_log[ntouch_log++] = (uint32_t)self->index << 16 | other->index;
}

static int count_touch(uint16_t self, uint16_t other)
{
	int i, n = 0;

	for (i = 0; i < ntouch_log; i++)
		n += touch_log[i] == ((uint32_t)self << 16 | other);
	return n;
}

static const float zero3[3] = { 0, 0, 0 };

/* stand the player at org (map-local) on the floor, zero state */
static void stand(const float org[3])
{
	float v[24];
	int i;

	memset(v, 0, sizeof(v));
	for (i = 0; i < 3; i++)
		v[1 + i] = org[i];
	v[10] = PL_FL_CLIENT;
	v[13] = 28;
	v[15] = COL_EMPTY;
	v[22] = PL_MOVE_WALK;
	reset_player(v, -1);
	/* settle onto the floor */
	for (i = 0; i < 10; i++)
		move(0.01f * (float)(i + 1), zero3, 0, 0, 0, 0, 10);
}

static int pe_has(int slot)
{
	int i;

	for (i = 0; i < phy_pm.npe; i++)
		if (phy_pm.pe[i].index == slot)
			return 1;
	return 0;
}

static void free_all(void)
{
	int i;

	for (i = 2; i < HL64_MAX_ENTITIES; i++)
		if (ent_get(i))
			ent_free(ent_get(i));
}

/* PM1: the adapter round trip, identity move (movetype NONE, msec 0) */
static void test_pm1(const float org[3])
{
	ent_player_t *pl = phy_player(), before;
	phy_pm_t *m = &phy_pm;
	float g[3];
	int k;

	stand(org);
	for (k = 0; k < 3; k++)
		g[k] = org[k];
	/* identity move: movetype NONE, msec 0, zero command */
	pl->movetype = PL_MOVE_NONE;
	for (k = 0; k < 3; k++)
	{
		pl->velocity[k] = 10.5f + (float)k;
		pl->basevelocity[k] = -3.25f * (float)(k + 1);
		pl->movedir[k] = 0.5f * (float)k;
		pl->punchangle[k] = 0;
	}
	pl->duck_time = 0;
	pl->in_duck = 0;
	pl->step_time = 123;
	pl->step_left = 1;
	pl->fall_velocity = 0;
	pl->swim_time = 77;
	pl->gravity = 0.5f;
	pl->friction = 0.75f;
	pl->waterjump_time = 0;
	pl->maxspeed = 0;
	/* FL_ONTRAIN: pm_shared leaves movetype NONE alone (no ladder reset) */
	pl->flags = PL_FL_CLIENT | PL_FL_GODMODE | PL_FL_NOTARGET | PL_FL_ONGROUND | PL_FL_ONTRAIN;
	before = *pl;
	memset(m->cmd.viewangles, 0, sizeof(m->cmd.viewangles));
	m->cmd.viewangles[0] = 10;
	m->cmd.viewangles[1] = 45;
	m->cmd.forwardmove = m->cmd.sidemove = m->cmd.upmove = 0;
	m->cmd.buttons = 0;
	m->cmd.msec = 0;
	m->pl = pl;
	m->time_ms = 1000;
	game_pm_move(m);
	for (k = 0; k < 3; k++)
	{
		CHECK(pl->origin[k] == before.origin[k] && fabsf(pl->origin[k] - g[k]) < 1, "PM1 origin");
		CHECK(pl->velocity[k] == before.velocity[k], "PM1 velocity[%d]", k);
		CHECK(pl->basevelocity[k] == before.basevelocity[k], "PM1 basevelocity[%d]", k);
		CHECK(pl->movedir[k] == before.movedir[k], "PM1 movedir[%d]", k);
		CHECK(pl->view_ofs[k] == before.view_ofs[k], "PM1 view_ofs[%d]", k);
		CHECK(pl->punchangle[k] == 0, "PM1 punchangle[%d]", k);
		CHECK(pl->mins[k] == before.mins[k] && pl->maxs[k] == before.maxs[k], "PM1 box");
	}
	CHECK(pl->view_angles[0] == 10 && pl->view_angles[1] == 45, "PM1 view_angles");
	CHECK(pl->angles[0] == 10 && pl->angles[1] == 45 && pl->angles[2] == 0, "PM1 angles");
	CHECK(pl->duck_time == before.duck_time && pl->in_duck == before.in_duck, "PM1 duck");
	CHECK(pl->step_time == 123 && pl->step_left == 1, "PM1 step sound state");
	CHECK(pl->fall_velocity == 0, "PM1 fall velocity");
	CHECK(pl->swim_time == 77, "PM1 swim time");
	CHECK(pl->flags == before.flags, "PM1 flags %lx vs %lx", (unsigned long)pl->flags,
		(unsigned long)before.flags);
	CHECK(pl->usehull == 0 && pl->gravity == 0.5f && pl->friction == 0.75f, "PM1 hull/gravity/friction");
	CHECK(pl->waterjump_time == 0 && pl->movetype == PL_MOVE_NONE, "PM1 waterjump/movetype");
	CHECK(pl->groundentity == 0, "PM1 ground entity %d", pl->groundentity);
	CHECK(pl->waterlevel == 0 && pl->watertype == COL_EMPTY, "PM1 water");
	CHECK(pl->oldbuttons == 0 && pl->buttons == 0 && m->ntouch == 0, "PM1 buttons/touch");
	pl->movetype = PL_MOVE_WALK;
	printf("PM1 done\n");
}

/* PM3 rules: start solid => fraction 0; all solid => start solid; hull per usehull */
static void test_pm3(const float org[3])
{
	static const int hull_of[4] = { 1, 3, 0, 2 };
	const wld_layer_t *L = wld_open(map);
	float s[3], e[3], zero[3] = { 0, 0, 0 };
	col_trace_t t, w;
	int u, k;

	stand(org);
	world_only();
	/* below the floor: the first point solid for hull 1 */
	for (k = 0; k < 3; k++)
		s[k] = e[k] = org[k];
	for (u = 64; u < 4096; u += 16)
	{
		s[2] = org[2] - (float)u;
		if (col_model_contents(COL_MODEL(L->slot, 0), zero, zero, 1, s) == COL_SOLID)
			break;
	}
	e[2] = s[2] - 10;
	CHECK(u < 4096, "PM3: no solid below the spot");
	phy_pm_trace(s, e, 0, 0, -1, &t);
	CHECK(t.startsolid && t.frac == 0 && t.end[2] == s[2], "PM3 start solid: ss %d frac %g",
		t.startsolid, (double)t.frac);
	CHECK(!t.allsolid || t.startsolid, "PM3 allsolid => startsolid");
	/* down onto the floor from the spot, each usehull */
	for (u = 0; u < 4; u++)
	{
		for (k = 0; k < 3; k++)
			s[k] = e[k] = org[k];
		e[2] -= 64;
		phy_pm_trace(s, e, 0, u, -1, &t);
		col_trace_model(COL_MODEL(L->slot, 0), zero, zero, hull_of[u], s, e, &w);
		if (w.allsolid)
			w.startsolid = 1;
		if (w.startsolid)
		{
			w.frac = 0;
			memcpy(w.end, s, sizeof(w.end));
		}
		CHECK(t.frac == w.frac && t.end[2] == w.end[2] && t.startsolid == w.startsolid
			&& (t.startsolid || t.normal[2] == w.normal[2]), "PM3 usehull %d: frac %g vs hull %d %g",
			u, (double)t.frac, hull_of[u], (double)w.frac);
	}
	printf("PM3 rules done\n");
}

/* the first submodel whose hull 0 holds its bounds' centre and the points
 * PM_CheckWater tests for a standing player there (feet + 1, waist, eyes) */
static int solid_submodel(float c[3])
{
	const wld_layer_t *L = wld_open(map);
	float mins[3], maxs[3], p[3];
	int sub, k, nsub = (int)L->dir->nsub;

	for (sub = 1; sub < nsub; sub++)
	{
		int ok = 1, dz;

		if (!col_model_bounds(COL_MODEL(L->slot, sub), mins, maxs))
			continue;
		for (k = 0; k < 3; k++)
			c[k] = 0.5f * (mins[k] + maxs[k]);
		for (dz = -35; dz <= 28 && ok; dz += dz < 0 ? 35 : 28)
		{
			memcpy(p, c, sizeof(p));
			p[2] += (float)dz;
			for (k = 0; k < 3; k++)
				p[k] += L->gofs[k];
			ok = col_model_contents(COL_MODEL(L->slot, sub), L->gofs, zero3, 0, p) == COL_SOLID;
		}
		if (ok && maxs[0] - mins[0] < 512 && maxs[1] - mins[1] < 512)
			return sub;
	}
	return -1;
}

/* PM4: which entities become physents; a water volume changes waterlevel
 * but not collision */
static void test_pm4(const float org[3])
{
	static const float box[2][3] = { { -16, -16, -16 }, { 16, 16, 16 } };
	float p[3], c[3], ang[3] = { 0, 0, 0 };
	ent_player_t *pl = phy_player();
	int sub, k;

	stand(org);
	free_all();
	memcpy(p, org, sizeof(p));
	p[0] += 40;
	place(10, -1, p, ang, box[0], box[1], COL_SOLID_BBOX, 0, 0, -1, 0);	/* crate */
	p[0] = org[0] - 40;
	place(13, -1, p, ang, box[0], box[1], COL_SOLID_NOT, 0, 0, -1, 0);
	p[0] = org[0];
	p[1] = org[1] + 40;
	place(12, -1, p, ang, box[0], box[1], COL_SOLID_TRIGGER, 0, 0, -1, 0);
	p[1] = org[1] - 40;
	place(14, -1, p, ang, box[0], box[1], COL_SOLID_BBOX, 0, 0, ENT_PLAYER, 0);	/* grenade */
	/* a door: a map submodel moved next to the player */
	sub = solid_submodel(c);
	CHECK(sub > 0, "PM4: no solid submodel in this map");
	if (sub > 0)
	{
		float mins[3], maxs[3];
		const wld_layer_t *L = wld_open(map);

		col_model_bounds(COL_MODEL(L->slot, sub), mins, maxs);
		for (k = 0; k < 3; k++)
			p[k] = org[k] - c[k];
		/* beside the player: its near face 8 u beyond the player's box */
		p[1] += -16 - 8 - (maxs[1] - c[1]);
		place(11, sub, p, ang, mins, maxs, COL_SOLID_BSP, 0, 0, -1, 0);
	}
	move(1.0f, zero3, 0, 0, 0, 0, 10);
	CHECK(pe_has(10), "PM4: crate not a physent");
	CHECK(sub <= 0 || pe_has(11), "PM4: door not a physent");
	CHECK(!pe_has(12), "PM4: trigger is a physent");
	CHECK(!pe_has(13), "PM4: SOLID_NOT is a physent");
	CHECK(!pe_has(14), "PM4: the player's grenade is a physent");
	CHECK(!pe_has(ENT_PLAYER), "PM4: the player is a physent");

	/* PM13: the submodel as a func_monsterclip around the player: never a
	 * physent, the player stands inside it */
	free_all();
	if (sub > 0)
	{
		float mins[3], maxs[3];
		const wld_layer_t *L = wld_open(map);

		col_model_bounds(COL_MODEL(L->slot, sub), mins, maxs);
		for (k = 0; k < 3; k++)
			p[k] = org[k] - c[k];
		place(17, sub, p, ang, mins, maxs, COL_SOLID_BSP, 0, 0, -1, COL_FL_MONSTERCLIP);
		move(1.05f, zero3, 0, 0, 0, 0, 10);
		CHECK(!pe_has(17), "PM13: func_monsterclip is a physent");
		CHECK(phy_pm_test(pl->origin, pl->usehull, NULL) == -1, "PM13: func_monsterclip blocks the player");
		printf("PM13 done\n");
	}

	/* water: the submodel as a contents brush around the player */
	free_all();
	if (sub > 0)
	{
		float mins[3], maxs[3], before[3];
		const wld_layer_t *L = wld_open(map);
		int frame;

		col_model_bounds(COL_MODEL(L->slot, sub), mins, maxs);
		for (k = 0; k < 3; k++)
			p[k] = org[k] - c[k];
		place(15, sub, p, ang, mins, maxs, COL_SOLID_NOT, COL_WATER, 0, -1, 0);
		memcpy(before, pl->origin, sizeof(before));
		for (frame = 0; frame < 5; frame++)
			move(1.1f + 0.01f * (float)frame, zero3, 0, 0, 0, 0, 10);
		CHECK(pe_has(15), "PM4: water brush not a physent");
		CHECK(pl->waterlevel >= 2 && pl->watertype == COL_WATER, "PM4: waterlevel %d type %d",
			pl->waterlevel, pl->watertype);
		CHECK(phy_pm_test(pl->origin, pl->usehull, NULL) == -1, "PM4: water blocks the player");
		ent_free(ent_get(15));
	}
	printf("PM4 done\n");
}

/* PM5: impact touches from the pmove touch list; trigger touches via linking */
static void test_pm5(const float org[3])
{
	static const float box[2][3] = { { -16, -16, -16 }, { 16, 16, 16 } };
	float p[3], ang[3] = { 0, 0, 0 };
	col_ent_t *crate, *trig;
	int frame, touching = 0;

	stand(org);
	free_all();
	memcpy(p, org, sizeof(p));
	p[0] += 48;
	p[2] -= 20;
	crate = place(10, -1, p, ang, box[0], box[1], COL_SOLID_BBOX, 0, 0, -1, 0);
	ent_set_touch(crate, 1);
	memcpy(p, org, sizeof(p));
	trig = place(12, -1, p, ang, box[0], box[1], COL_SOLID_TRIGGER, 0, 0, -1, 0);
	ent_set_touch(trig, 2);
	phy_set_touch_call(on_touch);
	for (frame = 0; frame < 40; frame++)
	{
		int i, contact = 0;

		ntouch_log = 0;
		move(2.0f + 0.01f * (float)frame, zero3, 400, 0, 0, IN_BTN_FORWARD, 10);
		for (i = 0; i < phy_pm.ntouch; i++)
			contact |= phy_pm.touch[i].ent >= 0 && phy_pm.pe[phy_pm.touch[i].ent].index == 10;
		CHECK(count_touch(10, ENT_PLAYER) == contact, "PM5 frame %d: crate touches %d, contact %d",
			frame, count_touch(10, ENT_PLAYER), contact);
		touching += contact;
		/* the trigger box (+1 u widening) still meets the player's box */
		if (fabsf(phy_player()->origin[0] - org[0]) < 30)
			CHECK(count_touch(12, ENT_PLAYER) == 1, "PM5 frame %d: trigger touches %d", frame,
				count_touch(12, ENT_PLAYER));
		CHECK(count_touch(ENT_PLAYER, 10) == 0, "PM5: the player has no touch function");
	}
	CHECK(touching > 10, "PM5: walked into the crate on %d frames only", touching);
	phy_set_touch_call(NULL);
	free_all();
	printf("PM5 done\n");
}

/* PM6: riding a rising platform. The pusher (M3, physics-movetypes.md)
 * moves the platform and carries the player; pmove keeps him on it. */
static void test_pm6(const float org[3])
{
	static const float lift[2][3] = { { -48, -48, -4 }, { 48, 48, 4 } };
	ent_player_t *pl = phy_player();
	float p[3], ang[3] = { 0, 0, 0 }, start[3];
	col_ent_t *e;
	int frame, k;

	stand(org);
	free_all();
	memcpy(start, org, sizeof(start));
	start[2] += 4;
	memcpy(p, start, sizeof(p));
	p[2] -= 36 + 4;		/* platform top at the player's feet */
	e = place(16, -1, p, ang, lift[0], lift[1], COL_SOLID_BBOX, 0, 0, -1, 0);
	stand(start);
	CHECK(pl->groundentity == 16, "PM6: not standing on the platform (%d)", pl->groundentity);
	for (frame = 0; frame < 12; frame++)
	{
		/* the pusher step: platform and rider up 1 u */
		e->origin[2] += 1;
		ent_link(e);
		pl->origin[2] += 1;
		move(3.0f + 0.01f * (float)frame, zero3, 0, 0, 0, 0, 10);
		CHECK((pl->flags & PL_FL_ONGROUND) && pl->groundentity == 16,
			"PM6 frame %d: off the platform (flags %lx ground %d)", frame,
			(unsigned long)pl->flags, pl->groundentity);
		CHECK(pl->velocity[2] == 0, "PM6 frame %d: velocity z %g", frame, (double)pl->velocity[2]);
	}
	for (k = 0; k < 2; k++)
		CHECK(fabsf(pl->origin[k] - start[k]) < 0.01f, "PM6: drifted on axis %d", k);
	CHECK(fabsf(pl->origin[2] - (e->origin[2] - gofs[2] + 4 + 36)) < 0.1f, "PM6: rider height %g",
		(double)(pl->origin[2] - (e->origin[2] - gofs[2])));
	free_all();
	printf("PM6 done\n");
}

/* PM10: on the ground the duck finishes on the first move whose timer
 * (1000 ms at the pressing move, less each later move's msec) is strictly
 * below 600 ms: the 41st move after the press at msec 10, the 13th at 33,
 * the 9th at 50 (at exactly 600 it does not) */
static void test_pm10(const float org[3])
{
	static const int msec[3] = { 10, 33, 50 }, want[3] = { 41, 13, 9 };
	ent_player_t *pl = phy_player();
	int i, n;

	for (i = 0; i < 3; i++)
	{
		stand(org);
		for (n = 0; n <= 60 && !(pl->flags & PL_FL_DUCKING); n++)
			move(5.0f + 0.001f * (float)(n * msec[i]), zero3, 0, 0, 0, IN_BTN_DUCK, msec[i]);
		CHECK(n - 1 == want[i], "PM10 msec %d: ducked on move %d after the press, want %d",
			msec[i], n - 1, want[i]);
	}
	printf("PM10 done\n");
}

/* PM7: 70 small boxes in the move box keep the nearest, log once */
static void test_pm7(const float org[3])
{
	static const float box[2][3] = { { -1, -1, -1 }, { 1, 1, 1 } };
	float p[3], ang[3] = { 0, 0, 0 }, d[70], cut;
	phy_player_prof_t pr;
	int i, n = 0, kept = 0;

	stand(org);
	free_all();
	for (i = 0; i < 70; i++)
	{
		p[0] = org[0] - 30 + 6 * (float)(i % 10);
		p[1] = org[1] - 30 + 10 * (float)(i / 10);
		p[2] = org[2] + 40 + (float)(i % 7);
		place(20 + i, -1, p, ang, box[0], box[1], COL_SOLID_BBOX, 0, 0, -1, 0);
	}
	phy_player_prof(&pr, 1);
	move(4.0f, zero3, 0, 0, 0, 0, 10);
	phy_player_prof(&pr, 1);
	CHECK(phy_pm.npe == HL64_PM_PHYSENTS, "PM7: %d physents", phy_pm.npe);
	CHECK(pr.overflow == 1, "PM7: overflow frames %lu", (unsigned long)pr.overflow);
	/* the kept boxes are the nearest: none dropped is nearer than a kept one */
	for (i = 0; i < 70; i++)
	{
		const col_ent_t *e = ent_get(20 + i);
		float dd = 0;
		int k;

		for (k = 0; k < 3; k++)
		{
			float q = phy_player()->origin[k] + gofs[k], v = q < e->absmin[k] ? e->absmin[k] - q
				: q > e->absmax[k] ? q - e->absmax[k] : 0;

			dd += v * v;
		}
		d[i] = dd;
	}
	cut = 0;
	for (i = 0; i < 70; i++)
		if (pe_has(20 + i))
		{
			kept++;
			if (d[i] > cut)
				cut = d[i];
		}
	for (i = 0; i < 70; i++)
		if (!pe_has(20 + i) && d[i] < cut)
			n++;
	CHECK(kept == HL64_PM_PHYSENTS - 1, "PM7: kept %d boxes", kept);
	CHECK(n == 0, "PM7: %d dropped boxes nearer than a kept one", n);
	/* ascending index kept */
	for (i = 2; i < phy_pm.npe; i++)
		CHECK(phy_pm.pe[i].index > phy_pm.pe[i - 1].index, "PM7: physent order");
	free_all();
	printf("PM7 done\n");
}

/* ---- script ------------------------------------------------------------------------ */

/* A selftest spot (map-local, standing): the standing box free, a floor
 * within 4 u, 18 u of headroom (PM6 lifts the player 4 + 12 u) and 96 u
 * clear ahead along +x (PM5 walks into a crate 48 u ahead); world only. */
static int spot_ok(const float org[3])
{
	float e[3];
	col_trace_t t;

	world_only();
	if (phy_pm_test(org, 0, NULL) != -1)
		return 0;
	memcpy(e, org, sizeof(e));
	e[2] -= 4;
	phy_pm_trace(org, e, 0, 0, -1, &t);
	if (t.frac == 1)
		return 0;
	e[2] = org[2] + 18;
	phy_pm_trace(org, e, 0, 0, -1, &t);
	if (t.frac < 1)
		return 0;
	e[2] = org[2];
	e[0] = org[0] + 96;
	phy_pm_trace(org, e, 0, 0, -1, &t);
	return t.frac == 1;
}

static int line(char *s)
{
	char *p;
	float v[32];
	int i;

	s[strcspn(s, "\r\n")] = 0;
	if (!strncmp(s, "map ", 4))
	{
		map = wld_map_find(s + 4);
		if (map < 0 || !wld_open(map))
			return -1;
		gofs = wld_map_origin(map);
		return 0;
	}
	if (map < 0)
		return -1;
	p = s;
	if (!strncmp(s, "ent ", 4))
	{
		int slot, sub, solid, skin, rmode, owner;

		p += 4;
		if (rdi(&p, &slot) || rdi(&p, &sub) || rd(&p, v, 12) || rdi(&p, &solid) || rdi(&p, &skin)
			|| rdi(&p, &rmode) || rdi(&p, &owner) || rd(&p, v + 12, 1) || slot < 2)
			return -1;
		return place(slot, sub, v, v + 3, v + 6, v + 9, solid, skin, rmode, owner,
			(uint32_t)v[12]) ? 0 : -1;
	}
	if (!strncmp(s, "free ", 5))
	{
		p += 5;
		if (rdi(&p, &i) || !ent_get(i) || i < 2)
			return -1;
		ent_free(ent_get(i));
		return 0;
	}
	if (!strncmp(s, "reset ", 6))
	{
		int ge;

		p += 6;
		if (rd(&p, v, 16) || rdi(&p, &ge) || rd(&p, v + 16, 8))
			return -1;
		reset_player(v, ge);
		return 0;
	}
	if (!strncmp(s, "cmd ", 4))
	{
		p += 4;
		if (rd(&p, v, 9))
			return -1;
		move(v[0], v + 1, v[4], v[5], v[6], (int)v[7], (int)v[8]);
		print_player();
		return 0;
	}
	if (!strncmp(s, "pmtrace ", 8))
	{
		p += 8;
		if (rdi(&p, &i) || i < 0 || i > 3 || rd(&p, v, 6))
			return -1;
		pmtrace(i, v, v + 3);
		return 0;
	}
	if (!strncmp(s, "selftest ", 9))
	{
		int found = 0;

		/* the first candidate spot with the room the tests need */
		p += 9;
		while (!found && !rd(&p, v, 3))
			found = spot_ok(v);
		if (!found)
		{
			printf("FAIL no clear selftest spot\nselftest FAIL\n");
			return 0;
		}
		test_pm1(v);
		test_pm3(v);
		test_pm4(v);
		test_pm5(v);
		test_pm6(v);
		test_pm7(v);
		test_pm10(v);
		printf(fails ? "selftest FAIL\n" : "selftest ok\n");
		return 0;
	}
	return s[0] ? -1 : 0;
}

int main(int argc, char **argv)
{
	char buf[4096];
	int ln = 0;

	if (argc != 3 || strcmp(argv[1], "--root"))
	{
		fprintf(stderr, "usage: hl64-pm --root CONTENT < script\n");
		return 2;
	}
	host_io_init(argv[2]);
	mem_boot_carve();
	settings_reset();
	io_init();
	dbg_init();
	if (!wld_init())
	{
		fprintf(stderr, "hl64-pm: no content in %s\n", argv[2]);
		return 2;
	}
	ent_init();
	in_init();
	phy_player_init();
	while (fgets(buf, sizeof(buf), stdin))
	{
		ln++;
		if (line(buf))
		{
			printf("FAIL script line %d: %s\n", ln, buf);
			return 1;
		}
	}
	return fails ? 1 : 0;
}
