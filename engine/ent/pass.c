/* SPDX-License-Identifier: MIT */
/*
 * pass.c -- the game clock, the active-set pass (docs/architecture.md 3.2
 * step 6, 6.3) and the per-entity step dispatch with the think rule and
 * the NONE / NOCLIP / FOLLOW steps (docs/specs/think-and-time.md 1-3,
 * physics-movetypes.md 2, 3).
 *
 * The pass pops every think due by time + dt into the due set, then walks
 * due | movers | kill in ascending slot order, re-reading the sets after
 * every step, so an entity created, set moving or made due during the walk
 * at a higher slot is stepped this frame (GoldSrc's ascending loop). The
 * motion movetypes (toss family, STEP, PUSH, PUSHSTEP) step through
 * phy_step_motion (engine/phys/move.c, push.c: slice Phys).
 */
#include <string.h>

#include "engine/core/sys.h"
#include "engine/ent/ent.h"
#include "engine/phys/phys.h"

ent_pass_t	ent_pass;

static phy_game_hooks_t hooks;
static struct {
	int		step_all;
	phy_ent_stats_t	st;
} P;

/* ---- clock (think-and-time.md 1) ------------------------------------------------ */

static struct {
	double	time;		/* frame start, f64 s */
	double	dt;		/* this frame's frame time */
	double	now, ft;	/* what eng_time() / eng_frametime() return */
	int	settle;		/* settle frames left (New Game) */
} clk = { 1.0, 0, 1.0, 0, 0 };

void eng_clock_new_game(void)
{
	clk.time = 1.0;
	clk.dt = 0;
	clk.settle = 2;
	eng_set_clock(clk.time, 0);
}

double eng_clock_frame(double dt)
{
	if (clk.settle > 0)
	{
		clk.settle--;
		dt = 0;
	}
	clk.dt = dt;
	eng_set_clock(clk.time, dt);
	return dt;
}

void eng_clock_advance(void)
{
	clk.time += clk.dt;
	eng_set_clock(clk.time, clk.dt);
}

double eng_clock_time(void)
{
	return clk.time;
}

void eng_set_clock(double time, double frametime)
{
	clk.now = time;
	clk.ft = frametime;
}

double eng_time(void)
{
	return clk.now;
}

double eng_frametime(void)
{
	return clk.ft;
}

/* ---- dispatch --------------------------------------------------------------------- */

void phy_set_game_hooks(const phy_game_hooks_t *h)
{
	if (h)
		hooks = *h;
	else
		memset(&hooks, 0, sizeof(hooks));
}

static int same(int e, uint16_t serial)
{
	return ent_is_live(e) && ent_c.id[e].serial == serial;
}

/* The think of e with eng_time() = t; 1 when e is still the same live
 * entity afterwards. */
static int dispatch_think(int e, double t)
{
	uint16_t serial = ent_c.id[e].serial;
	double now = clk.now, ft = clk.ft;
	int layer = eng_frame_layer();

	P.st.thinks++;
	if (!hooks.think || !ent_c.tm[e].think)
		return 1;
	eng_set_clock(t, ft);
	eng_set_frame_layer(ent_c.id[e].layer);
	hooks.think(e);
	eng_set_frame_layer(layer);
	eng_set_clock(now, ft);
	return same(e, serial);
}

int phy_run_think(int e, double time, double dt)
{
	ent_tm_t *tm = &ent_c.tm[e];
	float nt = tm->nextthink;
	double t;

	if (!(nt > 0) || (double)nt > time + dt)
		return 1;
	t = (double)nt < time ? time : (double)nt;
	tm->nextthink = 0;
	ent_sets_update(e);
	return dispatch_think(e, (double)(float)t);
}

int phy_call_think(int e, double t)
{
	ent_c.tm[e].nextthink = 0;
	ent_sets_update(e);
	return dispatch_think(e, t);
}

void phy_blocked(int e, int other)
{
	int layer = eng_frame_layer();

	if (!hooks.blocked || !ent_c.tm[e].blocked)
		return;
	eng_set_frame_layer(ent_c.id[e].layer);
	hooks.blocked(e, other);
	eng_set_frame_layer(layer);
}

void phy_dispatch_touch(int self, int other)
{
	int layer = eng_frame_layer();

	if (!hooks.touch || !ent_c.tm[self].touch)
		return;
	eng_set_frame_layer(ent_c.id[self].layer);
	hooks.touch(self, other);
	eng_set_frame_layer(layer);
}

/* ---- steps (physics-movetypes.md 3) ----------------------------------------------- */

static void step_noclip(int e, double dt)
{
	col_ent_t *v = &ent_c.cv[e];
	const ent_mot_t *m = &ent_c.mot[e];
	const float f = (float)dt;
	int k;

	for (k = 0; k < 3; k++)
	{
		v->angles[k] = v->angles[k] + f * m->avelocity[k];
		v->origin[k] = v->origin[k] + f * m->velocity[k];
	}
	ent_link(v);
}

static void step_follow(int e)
{
	col_ent_t *v = &ent_c.cv[e];
	int a = ent_c.phys[e].aiment, k;
	float d[3];

	if (!ent_is_live(a))
		return;
	ent_layer_delta(ent_c.id[a].layer, ent_c.id[e].layer, d);
	for (k = 0; k < 3; k++)
	{
		v->origin[k] = ent_c.cv[a].origin[k] + d[k];
		v->angles[k] = ent_c.cv[a].angles[k];
	}
	ent_link(v);
}

static void step(int e, double time, double dt)
{
	int mt = ent_c.cv[e].movetype, layer = eng_frame_layer();
	uint16_t serial = ent_c.id[e].serial;

	ent_sets_clear_due(e);
	P.st.stepped++;
	if (mt < 14)
		P.st.by_type[mt]++;
	eng_set_frame_layer(ent_c.id[e].layer);
	switch (mt)
	{
	case MOVETYPE_NONE:
	case MOVETYPE_WALK:	/* non-player WALK acts as NONE */
		phy_run_think(e, time, dt);
		break;
	case MOVETYPE_NOCLIP:
		if (phy_run_think(e, time, dt))
			step_noclip(e, dt);
		break;
	case MOVETYPE_FOLLOW:
		if (phy_run_think(e, time, dt))
			step_follow(e);
		break;
	case MOVETYPE_STEP:
	case MOVETYPE_FLY:
	case MOVETYPE_TOSS:
	case MOVETYPE_PUSH:
	case MOVETYPE_FLYMISSILE:
	case MOVETYPE_BOUNCE:
	case MOVETYPE_BOUNCEMISSILE:
	case MOVETYPE_PUSHSTEP:
		phy_step_motion(e, time, dt);
		break;
	default:
		sys_error("ent %d (%s): bad movetype %d", e, ent_class_name(e), mt);
	}
	eng_set_frame_layer(layer);
	if (!same(e, serial))
		return;
	if (ent_c.cv[e].flags & FL_KILLME)
		ent_destroy(e);
	else
		ent_sets_update(e);
}

void phy_entities_frame(double time, double dt)
{
	uint64_t t0 = sys_time_us();
	uint32_t us;
	int i, n;

	n = ent_heap_count();
	if ((uint32_t)n > P.st.heap_max)
		P.st.heap_max = (uint32_t)n;
	ent_heap_pop_due(time, dt);
	n = ent_movers_count();
	if ((uint32_t)n > P.st.movers_max)
		P.st.movers_max = (uint32_t)n;
	ent_pass.active = 1;
	ent_pass.time = time;
	ent_pass.dt = dt;
	for (i = ent_sets_next(0, P.step_all); i >= 0; i = ent_sets_next(i + 1, P.step_all))
	{
		ent_pass.cursor = i;
		if (i == ENT_PLAYER)
		{
			ent_sets_clear_due(i);	/* the player steps in step 5 */
			continue;
		}
		step(i, time, dt);
	}
	ent_pass.active = 0;
	ent_pass.cursor = -1;
	us = (uint32_t)(sys_time_us() - t0);
	P.st.us += us;
	if (us > P.st.us_max)
		P.st.us_max = us;
	P.st.frames++;
}

void phy_set_step_all(int on)
{
	P.step_all = on != 0;
}

void phy_ent_stats(phy_ent_stats_t *s, int reset)
{
	*s = P.st;
	if (reset)
		memset(&P.st, 0, sizeof(P.st));
}
