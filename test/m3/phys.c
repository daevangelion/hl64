/* SPDX-License-Identifier: MIT */
/*
 * phys.c -- host harness for docs/specs/physics-movetypes.md (P1-P15) and
 * think-and-time.md K4: the real movetype steps (engine/phys), the
 * active-set pass, engine/coll and the pmove host on the cooked maps,
 * driven by a line script on stdin. test/m3/phys.py rebuilds each GoldSrc
 * probe scenario (P-*, K-pusher, W-water) from its records and compares
 * the output; `selftest` runs the synthetic tests (P3 slides, P13 resting
 * counters, conveyor and frame checks) and prints "selftest ok" or FAIL
 * lines.
 *
 *   hl64-phys --root CONTENT < script
 *
 * Positions are map-local to the open map (entities are created in its
 * layer). Script lines (V = three floats):
 *   map NAME
 *   ent I SUB ORG ANG MINS MAXS SOLID MOVETYPE FLAGS SKIN RMODE OWNER PROBE
 *        slot I (GoldSrc's index), SUB brush model or -1; fields written
 *        directly (no pair check), linked without touch; PROBE 1: the
 *        probe's think / touch / blocked logging functions
 *   dyn I ORG ANG VEL AVEL BVEL FL GE LTIME NT GRAVITY FRICTION
 *        dynamic state (GE slot or -1); relinked without touch
 *   size I MINS MAXS | skin I S | solid I S
 *   think I MODE PERIOD COUNT   probe think mode: 0 log, 1 reschedule at
 *        (ltime for pushers, else time) + PERIOD, COUNT thinks (0 = no
 *        end), 3 stop (velocity and avelocity 0)
 *   conveyor I V
 *   player ORG FL GE            the probe bot as the player (standing)
 *   clock T                     game time (f64 from the f32 printed)
 *   frame DT [BOT]              one frame: BOT 1 = the player's zero
 *        command of 10 ms first; then the active-set pass; the clock
 *        advances. Prints "s I O A V AV BV FL GE LT NT WL WT MINS MAXS"
 *        for every tracked entity (the player from its block)
 *   track I...                  the entities printed after each frame
 *   free I
 *   walk I YAW DIST MODE        "wm RET O FL GE TRACEENT"
 *   drop I                      "drop RET O FL GE"
 *   mto I GOAL DIST             "mto RET O A IDEAL FL"
 *   bottom I                    "bottom RET"
 *   trace I START END           the move query with I's box and type:
 *                               "trace FRAC ENT ALLSOLID STARTSOLID END NORMAL"
 *   pair I SOLID MOVETYPE       ent_set_solid / ent_set_movetype (P15)
 *   prof                        "prof steps ..." roll-up of the motion
 * Events print "ev touch SELF OTHER", "ev think SELF T LTIME",
 * "ev blocked SELF OTHER LTIME".
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hl64/ent.h"
#include "hl64/grid.h"
#include "hl64/limits.h"
#include "hl64/pmove.h"
#include "hl64/settings.h"
#include "engine/core/dbg.h"
#include "engine/core/sys.h"
#include "engine/ent/ent.h"
#include "engine/in/in.h"
#include "engine/io/io.h"
#include "engine/mem/carve.h"
#include "engine/phys/motion.h"
#include "engine/phys/phys.h"
#include "engine/phys/player.h"
#include "engine/world/wld.h"
#include "plat/host/host.h"

static int map = -1, layer = -1;
static double now;
static int fails, quiet;
static uint16_t track[64];
static int ntrack;
static struct {
	uint8_t		mode;
	uint16_t	count, done;
	float		period;
} th[HL64_MAX_ENTITIES];

#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

static const float zero3[3];
static int16_t enemy_of[HL64_MAX_ENTITIES];

/* the probe monster's enemy (HLSDK pev->enemy) for FL_FLY / FL_SWIM moves */
int game_walk_enemy(int e)
{
	return enemy_of[e];
}

/* ---- the probe entity's functions ------------------------------------------- */

static void on_think(int e)
{
	ent_tm_t *tm = ent_tm(e);

	if (!quiet)
		printf("ev think %d %.9g %.9g\n", e, (double)(float)eng_time(), (double)tm->ltime);
	th[e].done++;
	switch (th[e].mode)
	{
	case 1:
		if (th[e].count == 0 || th[e].done < th[e].count)
			ent_think_at(e, (ent_cv(e)->movetype == MOVETYPE_PUSH ? tm->ltime
				: (float)eng_time()) + th[e].period);
		break;
	case 3:
		ent_set_velocity(e, zero3);
		ent_set_avelocity(e, zero3);
		break;
	}
}

static void on_touch(int e, int other)
{
	if (!quiet)
		printf("ev touch %d %d\n", e, other);
}

static void on_blocked(int e, int other)
{
	if (!quiet)
		printf("ev blocked %d %d %.9g\n", e, other, (double)ent_tm(e)->ltime);
}

/* ---- script helpers ---------------------------------------------------------------- */

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

static void pv(const float v[3])
{
	printf(" %.9g %.9g %.9g", (double)v[0], (double)v[1], (double)v[2]);
}

static void print_state(int i)
{
	const col_ent_t *v;
	const ent_mot_t *m;
	const ent_phys_t *p;

	if (i == ENT_PLAYER && phy_player_active())
	{
		const ent_player_t *pl = phy_player();

		printf("s %d", i);
		pv(pl->origin);
		pv(ent_cv(i)->angles);
		pv(pl->velocity);
		pv(zero3);
		pv(pl->basevelocity);
		printf(" %lu %d 0 0 %d %d", (unsigned long)pl->flags, pl->groundentity, pl->waterlevel,
			pl->watertype);
		pv(pl->mins);
		pv(pl->maxs);
		printf("\n");
		return;
	}
	if (!ent_is_live(i))
	{
		printf("s %d free\n", i);
		return;
	}
	v = ent_cv(i);
	m = ent_mot(i);
	p = ent_phys(i);
	printf("s %d", i);
	pv(v->origin);
	pv(v->angles);
	pv(m->velocity);
	pv(m->avelocity);
	pv(m->basevelocity);
	printf(" %lu %d %.9g %.9g %d %d", (unsigned long)v->flags,
		p->groundentity == ENT_NONE ? -1 : p->groundentity, (double)ent_tm(i)->ltime,
		(double)ent_tm(i)->nextthink, p->waterlevel, p->watertype);
	pv(v->mins);
	pv(v->maxs);
	printf("\n");
}

static col_ent_t *slot(int i)
{
	col_ent_t *e = ent_get(i);

	if (!e)
		e = ent_alloc_at(i);
	return e;
}

static void frame(double dt, int bot)
{
	if (bot && phy_player_active())
	{
		in_cmd_t c;

		memset(&c, 0, sizeof(c));
		c.msec = 10;
		eng_set_clock(now, dt);
		phy_player_frame(&c, now);
	}
	eng_set_clock(now, dt);
	phy_entities_frame(now, dt);
	now += dt;
	eng_set_clock(now, dt);
}

/* ---- self-contained tests ------------------------------------------------------------ */

/* A free spot (map-local): the 64-cube free and a floor within 2 u. */
static float spot[3];

static col_ent_t *box(int i, const float o[3], float half, int mt)
{
	col_ent_t *e = slot(i);
	float mn[3] = { -half, -half, 0 }, mx[3] = { half, half, 2 * half };

	ent_unlink(e);
	memcpy(e->origin, o, sizeof(e->origin));
	e->solid = COL_SOLID_BBOX;
	e->movetype = (uint8_t)mt;
	memcpy(e->mins, mn, sizeof(mn));
	memcpy(e->maxs, mx, sizeof(mx));
	ent_link(e);
	ent_motion_changed(i);
	return e;
}

/* P3 slides: a STEP box flying along a wall keeps the parallel component,
 * into a 90-degree corner stops, along a crease follows it. Walls are box
 * entities (SOLID_BBOX, MOVETYPE_NONE) built around the spot. */
static void test_slides(void)
{
	float o[3], wo[3], v[3];
	col_ent_t *m, *w1, *w2;
	int f;

	quiet = 1;
	/* wall at +x: the mover heads +x +y */
	memcpy(o, spot, sizeof(o));
	o[2] += 20;
	m = box(400, o, 8, MOVETYPE_STEP);
	m->flags = FL_FLY;
	memcpy(wo, o, sizeof(wo));
	wo[0] += 8 + 2 + 8;
	w1 = box(401, wo, 8, MOVETYPE_NONE);
	w1->mins[1] = -200;
	w1->maxs[1] = 200;
	ent_link(w1);
	v[0] = 100;
	v[1] = 50;
	v[2] = 0;
	ent_set_velocity(400, v);
	for (f = 0; f < 10; f++)
		frame(0.01, 0);
	CHECK(ent_mot(400)->velocity[0] == 0 && fabsf(ent_mot(400)->velocity[1] - 50) < 1e-4f,
		"P3 wall: velocity %g %g", (double)ent_mot(400)->velocity[0],
		(double)ent_mot(400)->velocity[1]);
	CHECK(fabsf(m->origin[0] + 8 - (wo[0] - 8)) <= 0.04f, "P3 wall: not at the wall (%g)",
		(double)(wo[0] - 8 - m->origin[0] - 8));
	CHECK(fabsf(m->origin[1] - (o[1] + 5)) < 0.01f, "P3 wall: parallel travel %g",
		(double)(m->origin[1] - o[1]));
	/* corner: a second wall at +y */
	memcpy(m->origin, o, sizeof(o));
	ent_link(m);
	memcpy(wo, o, sizeof(wo));
	wo[1] += 8 + 2 + 8;
	w2 = box(402, wo, 8, MOVETYPE_NONE);
	w2->mins[0] = -200;
	w2->maxs[0] = 200;
	ent_link(w2);
	ent_set_velocity(400, v);
	for (f = 0; f < 10; f++)
		frame(0.01, 0);
	CHECK(ent_mot(400)->velocity[0] == 0 && ent_mot(400)->velocity[1] == 0,
		"P3 corner: velocity %g %g", (double)ent_mot(400)->velocity[0],
		(double)ent_mot(400)->velocity[1]);
	/* crease: from inside the corner, a velocity into both walls with a
	 * vertical component meets both planes in one slide and follows their
	 * crease (the vertical line) */
	v[2] = 30;
	ent_set_velocity(400, v);
	frame(0.01, 0);
	CHECK(ent_mot(400)->velocity[0] == 0 && ent_mot(400)->velocity[1] == 0
		&& fabsf(ent_mot(400)->velocity[2] - 30) < 1e-4f, "P3 crease: velocity %g %g %g",
		(double)ent_mot(400)->velocity[0], (double)ent_mot(400)->velocity[1],
		(double)ent_mot(400)->velocity[2]);
	ent_destroy(400);
	ent_destroy(401);
	ent_destroy(402);
	quiet = 0;
	printf("P3 slides done\n");
}

/* P13: a resting crate, a resting gib and an idle func_wall are not
 * stepped; each reacts on the frame something moves it. */
static void test_p13(void)
{
	phy_motion_prof_t pr;
	phy_ent_stats_t st;
	float o[3], v[3] = { 0, 0, 0 };
	int f;

	quiet = 1;
	memcpy(o, spot, sizeof(o));
	o[2] += 4;
	box(410, o, 8, MOVETYPE_TOSS);
	o[0] += 24;
	box(411, o, 2, MOVETYPE_BOUNCE);
	ent_phys(411)->friction = 0.8f;
	o[0] += 24;
	box(412, o, 8, MOVETYPE_PUSH)->solid = COL_SOLID_BBOX;
	for (f = 0; f < 60; f++)
		frame(0.01, 0);
	CHECK((ent_cv(410)->flags & FL_ONGROUND) && (ent_cv(411)->flags & FL_ONGROUND),
		"P13: not at rest (%lx %lx)", (unsigned long)ent_cv(410)->flags,
		(unsigned long)ent_cv(411)->flags);
	phy_motion_prof(&pr, 1);
	phy_ent_stats(&st, 1);
	for (f = 0; f < 30; f++)
		frame(0.01, 0);
	phy_motion_prof(&pr, 1);
	phy_ent_stats(&st, 1);
	CHECK(pr.steps[PHY_K_TOSS] == 0 && pr.steps[PHY_K_PUSH] == 0 && st.stepped == 0,
		"P13: resting entities stepped (toss %u push %u pass %u)", pr.steps[PHY_K_TOSS],
		pr.steps[PHY_K_PUSH], st.stepped);
	printf("P13 counters toss=%u push=%u stepped=%u over 30 frames\n", pr.steps[PHY_K_TOSS],
		pr.steps[PHY_K_PUSH], st.stepped);
	/* something moves them */
	v[2] = 100;
	ent_set_velocity(410, v);
	ent_set_velocity(411, v);
	v[2] = 10;
	ent_set_velocity(412, v);
	ent_think_at(412, ent_tm(412)->ltime + 0.5f);
	frame(0.01, 0);
	CHECK(ent_cv(410)->origin[2] > spot[2] + 0.5f, "P13: crate did not react");
	CHECK(ent_cv(411)->origin[2] > spot[2] + 0.5f, "P13: gib did not react");
	CHECK(ent_cv(412)->origin[2] > o[2], "P13: wall did not react");
	phy_motion_prof(&pr, 1);
	CHECK(pr.steps[PHY_K_TOSS] == 2 && pr.steps[PHY_K_PUSH] == 1,
		"P13: wake steps toss %u push %u", pr.steps[PHY_K_TOSS], pr.steps[PHY_K_PUSH]);
	ent_destroy(410);
	ent_destroy(411);
	ent_destroy(412);
	quiet = 0;
	printf("P13 done\n");
}

/* ---- script --------------------------------------------------------------------------- */

static int line(char *s)
{
	char *p = s;
	float v[48];
	int i, k;

	s[strcspn(s, "\r\n")] = 0;
	if (!s[0])
		return 0;
	if (!strncmp(s, "map ", 4))
	{
		wld_layer_t *L;

		map = wld_map_find(s + 4);
		if (map < 0 || !(L = wld_open(map)))
			return -1;
		layer = L->slot;
		eng_set_frame_layer(layer);
		/* the world entity (slot 0), as the game spawns it */
		if (!ent_get(ENT_WORLD))
			ent_alloc_at(ENT_WORLD);
		return 0;
	}
	if (!strncmp(s, "clock ", 6))
	{
		p += 6;
		if (rd(&p, v, 1))
			return -1;
		now = (double)v[0];
		eng_set_clock(now, 0);
		return 0;
	}
	if (map < 0)
		return -1;
	if (!strncmp(s, "ent ", 4))
	{
		int sub, solid, mt, skin, rmode, owner, probe;
		unsigned long fl;
		col_ent_t *e;
		char *q;

		p += 4;
		if (rdi(&p, &i) || rdi(&p, &sub) || rd(&p, v, 12) || rdi(&p, &solid) || rdi(&p, &mt))
			return -1;
		fl = strtoul(p, &q, 10);
		p = q;
		if (rdi(&p, &skin) || rdi(&p, &rmode) || rdi(&p, &owner) || rdi(&p, &probe))
			return -1;
		if (!(e = slot(i)))
			return -1;
		ent_unlink(e);
		memcpy(e->origin, v, sizeof(e->origin));
		memcpy(e->angles, v + 3, sizeof(e->angles));
		memcpy(e->mins, v + 6, sizeof(e->mins));
		memcpy(e->maxs, v + 9, sizeof(e->maxs));
		e->model = sub >= 0 ? COL_MODEL(layer, sub) : COL_NONE;
		e->solid = (uint8_t)solid;
		e->movetype = (uint8_t)mt;
		e->flags = (uint32_t)fl;
		e->skin = (int16_t)skin;
		e->rendermode = (uint8_t)rmode;
		e->owner = owner >= 0 ? (uint16_t)owner : COL_NONE;
		ent_tm(i)->think = ent_tm(i)->touch = ent_tm(i)->blocked = probe ? 1 : 0;
		th[i].mode = 0;
		th[i].done = 0;
		/* as ent_set_movetype does (slice Ent): a motion movetype starts
		 * with watertype EMPTY (GoldSrc's first physics frame) */
		ent_phys(i)->watertype = (mt == MOVETYPE_STEP || mt == MOVETYPE_FLY
			|| mt == MOVETYPE_TOSS || mt == MOVETYPE_FLYMISSILE || mt == MOVETYPE_BOUNCE
			|| mt == MOVETYPE_BOUNCEMISSILE || mt == MOVETYPE_PUSHSTEP) ? COL_EMPTY : 0;
		ent_link(e);
		ent_motion_changed(i);
		return 0;
	}
	if (!strncmp(s, "dyn ", 4))
	{
		col_ent_t *e;
		unsigned long fl;
		int ge;
		char *q;

		p += 4;
		if (rdi(&p, &i) || !(e = ent_get(i)) || rd(&p, v, 15))
			return -1;
		fl = strtoul(p, &q, 10);
		p = q;
		if (rdi(&p, &ge) || rd(&p, v + 15, 4))
			return -1;
		memcpy(e->origin, v, sizeof(e->origin));
		memcpy(e->angles, v + 3, sizeof(e->angles));
		memcpy(ent_mot(i)->velocity, v + 6, 12);
		memcpy(ent_mot(i)->avelocity, v + 9, 12);
		memcpy(ent_mot(i)->basevelocity, v + 12, 12);
		e->flags = (uint32_t)fl;
		ent_phys(i)->groundentity = ge < 0 ? ENT_NONE : (uint16_t)ge;
		ent_tm(i)->ltime = v[15];
		ent_tm(i)->nextthink = v[16];
		ent_phys(i)->gravity = v[17];
		ent_phys(i)->friction = v[18];
		ent_link(e);
		ent_motion_changed(i);
		return 0;
	}
	if (!strncmp(s, "size ", 5))
	{
		p += 5;
		if (rdi(&p, &i) || !ent_get(i) || rd(&p, v, 6))
			return -1;
		ent_set_size(i, v, v + 3);
		return 0;
	}
	if (!strncmp(s, "solid ", 6) || !strncmp(s, "skin ", 5))
	{
		int x;

		p += s[1] == 'o' ? 6 : 5;
		if (rdi(&p, &i) || !ent_get(i) || rdi(&p, &x))
			return -1;
		if (s[1] == 'o')
			ent_cv(i)->solid = (uint8_t)x;
		else
			ent_cv(i)->skin = (int16_t)x;
		ent_link(ent_cv(i));
		return 0;
	}
	if (!strncmp(s, "pair ", 5))
	{
		int sol, mt;

		p += 5;
		if (rdi(&p, &i) || !ent_get(i) || rdi(&p, &sol) || rdi(&p, &mt))
			return -1;
		ent_set_movetype(i, mt);
		ent_set_solid(i, sol);
		printf("pair ok\n");
		return 0;
	}
	if (!strncmp(s, "setvel ", 7))
	{
		p += 7;
		if (rdi(&p, &i) || !ent_get(i) || rd(&p, v, 6))
			return -1;
		ent_set_velocity(i, v);
		ent_set_avelocity(i, v + 3);
		if (!rd(&p, v + 6, 1))
			ent_think_at(i, v[6]);
		return 0;
	}
	if (!strncmp(s, "setorg ", 7))
	{
		p += 7;
		if (rdi(&p, &i) || !ent_get(i) || rd(&p, v, 3))
			return -1;
		ent_set_origin(i, v);
		return 0;
	}
	if (!strncmp(s, "enemy ", 6))
	{
		int en;

		p += 6;
		if (rdi(&p, &i) || rdi(&p, &en) || i < 0 || i >= HL64_MAX_ENTITIES)
			return -1;
		enemy_of[i] = (int16_t)en;
		return 0;
	}
	if (!strncmp(s, "state ", 6))
	{
		unsigned long fl;
		int ge;
		char *q;

		p += 6;
		if (rdi(&p, &i) || !ent_get(i) || rd(&p, v, 3))
			return -1;
		fl = strtoul(p, &q, 10);
		p = q;
		if (rdi(&p, &ge))
			return -1;
		memcpy(ent_cv(i)->origin, v, sizeof(ent_cv(i)->origin));
		ent_cv(i)->flags = (uint32_t)fl;
		ent_phys(i)->groundentity = ge < 0 ? ENT_NONE : (uint16_t)ge;
		ent_phys(i)->yaw_speed = 360;
		memset(ent_mot(i)->velocity, 0, sizeof(ent_mot(i)->velocity));
		ent_link(ent_cv(i));
		ent_motion_changed(i);
		return 0;
	}
	if (!strncmp(s, "think ", 6))
	{
		int mode, count;

		p += 6;
		if (rdi(&p, &i) || rdi(&p, &mode) || rd(&p, v, 1) || rdi(&p, &count))
			return -1;
		th[i].mode = (uint8_t)mode;
		th[i].period = v[0];
		th[i].count = (uint16_t)count;
		th[i].done = 0;
		return 0;
	}
	if (!strncmp(s, "conveyor ", 9))
	{
		p += 9;
		if (rdi(&p, &i) || rd(&p, v, 3))
			return -1;
		phy_set_conveyor(i, v);
		return 0;
	}
	if (!strncmp(s, "player ", 7))
	{
		unsigned long fl;
		int ge;
		char *q;
		ent_player_t *pl;

		p += 7;
		if (rd(&p, v, 3))
			return -1;
		fl = strtoul(p, &q, 10);
		p = q;
		if (rdi(&p, &ge))
			return -1;
		phy_player_spawn(map, v, zero3);
		pl = phy_player();
		pl->flags = (uint32_t)fl;
		pl->groundentity = (int16_t)ge;
		pl->movetype = PL_MOVE_WALK;
		ent_link(ent_cv(ENT_PLAYER));
		return 0;
	}
	if (!strncmp(s, "frame ", 6))
	{
		int bot = 0;

		p += 6;
		if (rd(&p, v, 1))
			return -1;
		rdi(&p, &bot);
		frame((double)v[0], bot);
		for (k = 0; k < ntrack; k++)
			print_state(track[k]);
		printf("end\n");
		return 0;
	}
	if (!strncmp(s, "track", 5))
	{
		p += 5;
		ntrack = 0;
		while (ntrack < 64 && !rdi(&p, &i))
			track[ntrack++] = (uint16_t)i;
		return 0;
	}
	if (!strncmp(s, "free ", 5))
	{
		p += 5;
		if (rdi(&p, &i))
			return -1;
		ent_destroy(i);
		return 0;
	}
	if (!strncmp(s, "walk ", 5))
	{
		int mode, r;

		p += 5;
		if (rdi(&p, &i) || !ent_get(i) || rd(&p, v, 2) || rdi(&p, &mode))
			return -1;
		r = phy_walk_move(i, v[0], v[1], mode);
		printf("wm %d", r);
		pv(ent_cv(i)->origin);
		printf(" %lu %d %d\n", (unsigned long)ent_cv(i)->flags,
			ent_phys(i)->groundentity == ENT_NONE ? -1 : ent_phys(i)->groundentity,
			phy_trace_ent());
		return 0;
	}
	if (!strncmp(s, "drop ", 5))
	{
		int r;

		p += 5;
		if (rdi(&p, &i) || !ent_get(i))
			return -1;
		r = phy_drop_to_floor(i);
		printf("drop %d", r);
		pv(ent_cv(i)->origin);
		printf(" %lu %d\n", (unsigned long)ent_cv(i)->flags,
			ent_phys(i)->groundentity == ENT_NONE ? -1 : ent_phys(i)->groundentity);
		return 0;
	}
	if (!strncmp(s, "mto ", 4))
	{
		int r;

		p += 4;
		if (rdi(&p, &i) || !ent_get(i) || rd(&p, v, 4))
			return -1;
		r = phy_move_to_origin(i, v, v[3], WALKMOVE_NORMAL);
		printf("mto %d", r);
		pv(ent_cv(i)->origin);
		pv(ent_cv(i)->angles);
		printf(" %.9g %lu\n", (double)ent_phys(i)->ideal_yaw, (unsigned long)ent_cv(i)->flags);
		return 0;
	}
	if (!strncmp(s, "trace ", 6))
	{
		col_trace_t tr;
		col_ent_t *e;

		p += 6;
		if (rdi(&p, &i) || !(e = ent_get(i)) || rd(&p, v, 6))
			return -1;
		col_move(v, e->mins, e->maxs, v + 3, phy_move_type(e), e, &tr);
		printf("trace %.9g %d %d %d", (double)tr.frac, tr.ent, tr.allsolid, tr.startsolid);
		pv(tr.end);
		pv(tr.normal);
		printf("\n");
		return 0;
	}
	if (!strncmp(s, "bottom ", 7))
	{
		p += 7;
		if (rdi(&p, &i) || !ent_get(i))
			return -1;
		printf("bottom %d\n", phy_check_bottom(i));
		return 0;
	}
	if (!strncmp(s, "prof", 4))
	{
		phy_motion_prof_t pr;

		phy_motion_prof(&pr, 1);
		printf("prof frames=%u us=%u max=%u toss=%u step=%u push=%u pushstep=%u traces=%u\n",
			pr.frames, pr.us, pr.us_max, pr.steps[PHY_K_TOSS], pr.steps[PHY_K_STEP],
			pr.steps[PHY_K_PUSH], pr.steps[PHY_K_PUSHSTEP], pr.traces);
		return 0;
	}
	if (!strncmp(s, "selftest ", 9))
	{
		p += 9;
		if (rd(&p, spot, 3))
			return -1;
		test_slides();
		test_p13();
		printf(fails ? "selftest FAIL\n" : "selftest ok\n");
		return 0;
	}
	return -1;
}

int main(int argc, char **argv)
{
	static const phy_game_hooks_t hooks = { on_think, on_touch, on_blocked };
	char buf[2048];
	int ln = 0;

	if (argc != 3 || strcmp(argv[1], "--root"))
	{
		fprintf(stderr, "usage: hl64-phys --root CONTENT < script\n");
		return 2;
	}
	host_io_init(argv[2]);
	mem_boot_carve();
	settings_reset();
	io_init();
	dbg_init();
	if (!wld_init())
	{
		fprintf(stderr, "hl64-phys: no content in %s\n", argv[2]);
		return 2;
	}
	ent_init();
	in_init();
	phy_player_init();
	phy_set_game_hooks(&hooks);
	memset(enemy_of, 0xFF, sizeof(enemy_of));
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
