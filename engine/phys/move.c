/* SPDX-License-Identifier: MIT */
/*
 * move.c -- the motion movetypes' per-frame steps
 * (docs/specs/physics-movetypes.md 2, 3, 5): the toss family (TOSS,
 * BOUNCE, FLY, FLYMISSILE, BOUNCEMISSILE), STEP and PUSHSTEP; PUSH goes to
 * push.c. Base velocity, gravity, the push and slide moves, ground
 * friction, the 1 u ground probe and the water level live here.
 *
 * Arithmetic: positions and velocities stay float as GoldSrc's entvars;
 * the frame time is f64, and products with it are formed in double and
 * stored to float once, as the probe captures show.
 */
#include <math.h>
#include <string.h>

#include "hl64/ent.h"
#include "hl64/settings.h"
#include "engine/core/log.h"
#include "engine/core/sys.h"
#include "engine/ent/ent.h"
#include "engine/phys/motion.h"
#include "engine/phys/phys.h"

/* calibrated constants (physics-movetypes.md 3, 5) */
#define FLOOR_Z		0.7f	/* a floor plane: normal.z above this */
#define BOUNCE_STOP2	900.0f	/* BOUNCE at rest below 30 u/s */
#define CREEP		0.9	/* BOUNCE: share of the frame's rest after a floor bounce */
#define WATER_DRAG	0.8	/* STEP: speed lost per s per water level */
#define SLIDE_BUMPS	4
#define SLIDE_PLANES	5

uint32_t phy_trace_count;

static struct {
	phy_motion_prof_t	p;
	double			frame_time;	/* the frame being summed */
	uint32_t		frame_us;
	int			in_frame;
} prof;

/* conveyors: speed * movedir of FL_CONVEYOR pushers */
#define CONVEYORS	16
static struct {
	ent_h	h;
	float	v[3];
} conv[CONVEYORS];

static const float zero3[3];

static int nonzero(const float v[3])
{
	return v[0] != 0 || v[1] != 0 || v[2] != 0;
}

int phy_alive(int e, uint16_t serial)
{
	return ent_is_live(e) && ent_c.id[e].serial == serial;
}

void phy_trace(const float start[3], const float mins[3], const float maxs[3],
	const float end[3], int type, const col_ent_t *pass, col_trace_t *tr)
{
	phy_trace_count++;
	col_move(start, mins, maxs, end, type, pass, tr);
}

int phy_move_type(const col_ent_t *v)
{
	if (v->movetype == MOVETYPE_FLYMISSILE || v->movetype == MOVETYPE_BOUNCEMISSILE)
		return COL_MOVE_MISSILE;
	if (v->solid == COL_SOLID_TRIGGER || v->solid == COL_SOLID_NOT)
		return COL_MOVE_NOMONSTERS;
	return COL_MOVE_NORMAL;
}

void phy_clip(const float in[3], const float n[3], float out[3], float k)
{
	float back = (in[0] * n[0] + in[1] * n[1] + in[2] * n[2]) * k;
	int i;

	for (i = 0; i < 3; i++)
	{
		out[i] = in[i] - n[i] * back;
		if (out[i] > -0.1f && out[i] < 0.1f)
			out[i] = 0;
	}
}

int phy_shove(int e, const float d[3], col_trace_t *tr)
{
	col_ent_t *v = &ent_c.cv[e];
	uint16_t serial = ent_c.id[e].serial;
	float end[3];
	int k;

	for (k = 0; k < 3; k++)
		end[k] = v->origin[k] + d[k];
	phy_trace(v->origin, v->mins, v->maxs, end, phy_move_type(v), v, tr);
	memcpy(v->origin, tr->end, sizeof(v->origin));
	phy_link_touch(v);
	if (!phy_alive(e, serial))
		return 0;
	if (tr->ent == ENT_WORLD || (tr->ent > 0 && ent_is_live(tr->ent)))
		phy_impact(v, &ent_c.cv[tr->ent]);
	return phy_alive(e, serial);
}

void phy_set_conveyor(int e, const float vel[3])
{
	int i, slot = -1;

	for (i = 0; i < CONVEYORS; i++)
	{
		int c = conv[i].h ? ent_of(conv[i].h) : -1;

		if (c == e)
		{
			slot = i;
			break;
		}
		if (c < 0 && slot < 0)
			slot = i;
	}
	if (slot < 0)
		sys_error("phys: more than %d conveyors (ent %d, %s)", CONVEYORS, e, ent_class_name(e));
	conv[slot].h = nonzero(vel) ? ent_handle_of(e) : 0;
	memcpy(conv[slot].v, vel, sizeof(conv[slot].v));
}

static const float *conveyor_of(int g)
{
	int i;

	for (i = 0; i < CONVEYORS; i++)
		if (conv[i].h && ent_of(conv[i].h) == g)
			return conv[i].v;
	return zero3;
}

/* ---- common operations (spec 2) ------------------------------------------- */

/* Conveyor ground, then the base velocity fold on the frame its flag is
 * clear: velocity += basevelocity * (1 + dt/2) [probe:measured P-toss]. */
static void base_velocity(int e, double dt)
{
	col_ent_t *v = &ent_c.cv[e];
	ent_mot_t *m = &ent_c.mot[e];
	int g = ent_c.phys[e].groundentity, k;

	/* a freed ground: nothing holds it any more */
	if (g != ENT_NONE && g != ENT_WORLD && !ent_is_live(g))
	{
		ent_c.phys[e].groundentity = ENT_NONE;
		v->flags &= ~FL_ONGROUND;
		g = ENT_NONE;
	}

	if ((v->flags & FL_ONGROUND) && g != ENT_NONE && (ent_c.cv[g].flags & FL_CONVEYOR))
	{
		const float *c = conveyor_of(g);

		for (k = 0; k < 3; k++)
			m->basevelocity[k] = (v->flags & FL_BASEVELOCITY) ? m->basevelocity[k] + c[k] : c[k];
		v->flags |= FL_BASEVELOCITY;
	}
	if (!(v->flags & FL_BASEVELOCITY) && nonzero(m->basevelocity))
	{
		for (k = 0; k < 3; k++)
		{
			m->velocity[k] = (float)((double)m->velocity[k]
				+ (double)m->basevelocity[k] * (1.0 + dt * 0.5));
			m->basevelocity[k] = 0;
		}
	}
	v->flags &= ~FL_BASEVELOCITY;
}

static void check_velocity(int e)
{
	float *vel = ent_c.mot[e].velocity, mx = settings.maxvelocity;
	int k;

	for (k = 0; k < 3; k++)
	{
		if (vel[k] != vel[k])
		{
			log_printf("phys: ent %d (%s) velocity NaN\n", e, ent_class_name(e));
			vel[k] = 0;
		}
		if (vel[k] > mx)
			vel[k] = mx;
		else if (vel[k] < -mx)
			vel[k] = -mx;
	}
}

static void add_gravity(int e, double dt)
{
	float s = ent_c.phys[e].gravity, *vel = ent_c.mot[e].velocity;

	if (s == 0)
		s = 1;
	vel[2] = (float)((double)vel[2] - (double)s * (double)settings.gravity * dt);
}

/* Collide and slide over t seconds (spec 2). */
static void slide(int e, double t)
{
	col_ent_t *v = &ent_c.cv[e];
	float *vel = ent_c.mot[e].velocity;
	float planes[SLIDE_PLANES][3], ref[3], first[3];
	uint16_t serial = ent_c.id[e].serial;
	int bump, np = 0, i, j, k;
	double left = t;

	memcpy(ref, vel, sizeof(ref));
	memcpy(first, vel, sizeof(first));
	for (bump = 0; bump < SLIDE_BUMPS; bump++)
	{
		col_trace_t tr;
		float end[3], nv[3];

		if (!nonzero(vel))
			break;
		for (k = 0; k < 3; k++)
			end[k] = (float)((double)v->origin[k] + left * (double)vel[k]);
		phy_trace(v->origin, v->mins, v->maxs, end, phy_move_type(v), v, &tr);
		if (tr.allsolid)
		{
			memset(vel, 0, 3 * sizeof(float));
			return;
		}
		if (tr.frac > 0)
		{
			memcpy(v->origin, tr.end, sizeof(v->origin));
			memcpy(ref, vel, sizeof(ref));
			np = 0;
		}
		if (tr.frac == 1)
			break;
		if (tr.normal[2] > FLOOR_Z && (tr.ent == ENT_WORLD
			|| (tr.ent > 0 && ent_c.cv[tr.ent].solid == COL_SOLID_BSP)))
		{
			v->flags |= FL_ONGROUND;
			ent_c.phys[e].groundentity = (uint16_t)tr.ent;
		}
		if (tr.ent == ENT_WORLD || (tr.ent > 0 && ent_is_live(tr.ent)))
		{
			ent_link(v);
			phy_impact(v, &ent_c.cv[tr.ent]);
			if (!phy_alive(e, serial))
				return;
		}
		left -= left * (double)tr.frac;
		if (np >= SLIDE_PLANES)
		{
			memset(vel, 0, 3 * sizeof(float));
			break;
		}
		memcpy(planes[np++], tr.normal, sizeof(planes[0]));
		for (i = 0; i < np; i++)
		{
			phy_clip(ref, planes[i], nv, 1);
			for (j = 0; j < np; j++)
				if (j != i && nv[0] * planes[j][0] + nv[1] * planes[j][1]
					+ nv[2] * planes[j][2] < 0)
					break;
			if (j == np)
				break;
		}
		if (i < np)
			memcpy(vel, nv, sizeof(nv));
		else if (np == 2)
		{
			float c[3], d;

			c[0] = planes[0][1] * planes[1][2] - planes[0][2] * planes[1][1];
			c[1] = planes[0][2] * planes[1][0] - planes[0][0] * planes[1][2];
			c[2] = planes[0][0] * planes[1][1] - planes[0][1] * planes[1][0];
			d = c[0] * vel[0] + c[1] * vel[1] + c[2] * vel[2];
			for (k = 0; k < 3; k++)
				vel[k] = c[k] * d;
		}
		else
		{
			memset(vel, 0, 3 * sizeof(float));
			break;
		}
		if (vel[0] * first[0] + vel[1] * first[1] + vel[2] * first[2] <= 0)
		{
			memset(vel, 0, 3 * sizeof(float));
			break;
		}
	}
}

/* ---- water level (spec 5) --------------------------------------------------- */

static int liquid(int c)
{
	return c <= COL_WATER && c >= COL_CURRENT_DOWN;
}

/* Feet (bottom of the linked box + 1, box centre in x and y), waist (box
 * centre), eyes (origin + view_ofs). Entering liquid halves velocity.z. */
static void water_level(int e)
{
	col_ent_t *v = &ent_c.cv[e];
	ent_phys_t *p = &ent_c.phys[e];
	int old = p->waterlevel, c;
	float pt[3];

	pt[0] = (v->absmin[0] + v->absmax[0]) * 0.5f;
	pt[1] = (v->absmin[1] + v->absmax[1]) * 0.5f;
	pt[2] = v->absmin[2] + 1;
	c = col_contents(pt);
	p->watertype = COL_EMPTY;
	p->waterlevel = 0;
	if (liquid(c))
	{
		p->watertype = (int8_t)c;
		p->waterlevel = 1;
		pt[2] = (v->absmin[2] + v->absmax[2]) * 0.5f;
		if (liquid(col_contents(pt)))
		{
			p->waterlevel = 2;
			pt[0] = v->origin[0];
			pt[1] = v->origin[1];
			pt[2] = v->origin[2] + (float)p->view_ofs_z;
			if (liquid(col_contents(pt)))
				p->waterlevel = 3;
		}
	}
	if (old == 0 && p->waterlevel > 0)
		ent_c.mot[e].velocity[2] *= 0.5f;
}

/* ---- toss family (spec 3) ---------------------------------------------------- */

static void step_toss(int e, double time, double dt)
{
	col_ent_t *v = &ent_c.cv[e];
	ent_mot_t *m = &ent_c.mot[e];
	ent_phys_t *p = &ent_c.phys[e];
	int mt = v->movetype, g, k;
	float d[3], ob;
	col_trace_t tr;

	base_velocity(e, dt);
	if (!phy_run_think(e, time, dt))
		return;
	if (m->velocity[2] > 0)
		v->flags &= ~FL_ONGROUND;
	/* resting on a monster or the player, or on a freed ground: no rest
	 * (lands again every frame) [probe:measured P-toss] */
	g = p->groundentity;
	if ((v->flags & FL_ONGROUND) && g != ENT_WORLD && g != ENT_NONE
		&& (ent_c.cv[g].flags & (FL_MONSTER | FL_CLIENT)))
		v->flags &= ~FL_ONGROUND;
	if ((v->flags & FL_ONGROUND) && !nonzero(m->velocity))
	{
		memset(m->avelocity, 0, sizeof(m->avelocity));
		if (!nonzero(m->basevelocity))
			goto water;
	}
	check_velocity(e);
	if (mt != MOVETYPE_FLY && mt != MOVETYPE_FLYMISSILE && mt != MOVETYPE_BOUNCEMISSILE)
		add_gravity(e, dt);
	for (k = 0; k < 3; k++)
	{
		v->angles[k] = (float)((double)v->angles[k] + (double)m->avelocity[k] * dt);
		d[k] = (float)((double)(m->velocity[k] + m->basevelocity[k]) * dt);
	}
	if (!phy_shove(e, d, &tr))
		return;
	if (tr.allsolid)
	{
		/* trapped in solid: it stops (and still sinks by its frame's
		 * gravity move each frame) [probe:measured P-toss, P-step] */
		memset(m->velocity, 0, sizeof(m->velocity));
		memset(m->avelocity, 0, sizeof(m->avelocity));
		goto water;
	}
	if (tr.frac == 1)
		goto water;
	ob = mt == MOVETYPE_BOUNCE ? 2 - p->friction : mt == MOVETYPE_BOUNCEMISSILE ? 2 : 1;
	phy_clip(m->velocity, tr.normal, m->velocity, ob);
	if (tr.normal[2] > FLOOR_Z)
	{
		int hit = tr.ent < 0 ? 0 : tr.ent;

		if (mt == MOVETYPE_BOUNCE || mt == MOVETYPE_BOUNCEMISSILE)
		{
			float sp[3];

			/* a bounce that no longer lifts it: vertical stop only */
			if ((double)m->velocity[2] < (double)settings.gravity * dt)
			{
				v->flags |= FL_ONGROUND;
				p->groundentity = (uint16_t)hit;
				m->velocity[2] = 0;
			}
			for (k = 0; k < 3; k++)
				sp[k] = m->velocity[k] + m->basevelocity[k];
			if (sp[0] * sp[0] + sp[1] * sp[1] + sp[2] * sp[2] >= BOUNCE_STOP2)
			{
				/* the rest of the frame, slowed [probe:measured P-bounce] */
				for (k = 0; k < 3; k++)
					d[k] = (float)((double)m->velocity[k] * (1.0 - (double)tr.frac)
						* dt * CREEP);
				if (!phy_shove(e, d, &tr))
					return;
				goto water;
			}
		}
		v->flags |= FL_ONGROUND;
		p->groundentity = (uint16_t)hit;
		memset(m->velocity, 0, sizeof(m->velocity));
		memset(m->avelocity, 0, sizeof(m->avelocity));
	}
water:
	water_level(e);
}

/* ---- STEP and PUSHSTEP (spec 3) ------------------------------------------------- */

/* Ground friction on the horizontal velocity when grounded; moving
 * horizontally consumes the entity's friction scale, which is then 1,
 * grounded or not [probe:measured P-pushable, P-step]. */
static void ground_friction(int e, double dt, int grounded)
{
	float *vel = ent_c.mot[e].velocity, *fr = &ent_c.phys[e].friction;
	double s = sqrt((double)vel[0] * vel[0] + (double)vel[1] * vel[1]);
	double f, control, ns;

	if (s <= 0)
		return;
	f = (double)settings.friction * (double)*fr;
	*fr = 1;
	if (!grounded)
		return;
	control = s < settings.stopspeed ? (double)settings.stopspeed : s;
	ns = s - dt * control * f;
	if (ns < 0)
		ns = 0;
	vel[0] = (float)(vel[0] * (ns / s));
	vel[1] = (float)(vel[1] * (ns / s));
}

/* On the ground after the move: a bottom corner, 1 u under the box, in
 * world SOLID (brush entities are not seen: on a platform FL_ONGROUND
 * alternates frame by frame; a corner over a slope counts while the
 * rest of the box is in the air) [probe:measured P-step, P-pushable]. */
static int corner_in_world(const col_ent_t *v)
{
	float p[3];
	int x, y;

	p[2] = v->origin[2] + v->mins[2] - 1;
	for (x = 0; x < 2; x++)
		for (y = 0; y < 2; y++)
		{
			p[0] = v->origin[0] + (x ? v->maxs[0] : v->mins[0]);
			p[1] = v->origin[1] + (y ? v->maxs[1] : v->mins[1]);
			if (col_contents(p) == COL_SOLID)
				return 1;
		}
	return 0;
}

/* The moving part of STEP / PUSHSTEP: ground friction when the entity
 * was on the ground at the frame start and check_bottom holds there, the
 * slide (a BSP floor sets FL_ONGROUND), else the corner test after the
 * move decides FL_ONGROUND; link with touch. */
static void ground_move(int e, double dt, int was)
{
	col_ent_t *v = &ent_c.cv[e];
	uint16_t serial = ent_c.id[e].serial;

	ground_friction(e, dt, was && phy_check_bottom(e));
	v->flags &= ~FL_ONGROUND;
	check_velocity(e);
	slide(e, dt);
	if (!phy_alive(e, serial))
		return;
	if (!(v->flags & FL_ONGROUND) && corner_in_world(v))
		v->flags |= FL_ONGROUND;
	phy_link_touch(v);
}

static void step_monster(int e, double time, double dt)
{
	col_ent_t *v = &ent_c.cv[e];
	ent_mot_t *m = &ent_c.mot[e];
	ent_phys_t *p = &ent_c.phys[e];
	uint16_t serial = ent_c.id[e].serial;
	int was = (v->flags & FL_ONGROUND) != 0, k;

	base_velocity(e, dt);
	/* in water: FL_INWATER and drag by the water level, gravity only at
	 * level 1 [probe:measured P-step] */
	if (p->waterlevel > 0)
	{
		double drag = 1.0 - dt * WATER_DRAG * (double)p->waterlevel;

		v->flags |= FL_INWATER;
		for (k = 0; k < 3; k++)
			m->velocity[k] = (float)((double)m->velocity[k] * drag);
	}
	else
		v->flags &= ~FL_INWATER;
	if (!was && !(v->flags & FL_FLY) && !((v->flags & FL_SWIM) && p->waterlevel > 0)
		&& p->waterlevel < 2)
		add_gravity(e, dt);
	if (nonzero(m->velocity) || nonzero(m->basevelocity))
	{
		ground_move(e, dt, was);
		if (!phy_alive(e, serial))
			return;
	}
	if (!phy_run_think(e, time, dt))
		return;
	water_level(e);
}

static void step_pushstep(int e, double time, double dt)
{
	col_ent_t *v = &ent_c.cv[e];
	ent_mot_t *m = &ent_c.mot[e];
	uint16_t serial = ent_c.id[e].serial;
	int was;

	base_velocity(e, dt);
	was = (v->flags & FL_ONGROUND) != 0;
	if (!was)
		add_gravity(e, dt);
	if (nonzero(m->velocity) || nonzero(m->basevelocity))
	{
		ground_move(e, dt, was);
		if (!phy_alive(e, serial))
			return;
	}
	if (!phy_run_think(e, time, dt))
		return;
	water_level(e);
}

/* ---- dispatch ----------------------------------------------------------------------- */

void phy_step_motion(int e, double time, double dt)
{
	uint64_t t0 = sys_time_us();
	uint32_t us;
	int kind;

	if (!prof.in_frame || time != prof.frame_time)
	{
		if (prof.in_frame && prof.frame_us > prof.p.us_max)
			prof.p.us_max = prof.frame_us;
		prof.in_frame = 1;
		prof.frame_time = time;
		prof.frame_us = 0;
		prof.p.frames++;
	}
	switch (ent_c.cv[e].movetype)
	{
	case MOVETYPE_PUSH:
		kind = PHY_K_PUSH;
		phy_pusher_step(e, time, dt);
		break;
	case MOVETYPE_STEP:
		kind = PHY_K_STEP;
		step_monster(e, time, dt);
		break;
	case MOVETYPE_PUSHSTEP:
		kind = PHY_K_PUSHSTEP;
		step_pushstep(e, time, dt);
		break;
	default:
		kind = PHY_K_TOSS;
		step_toss(e, time, dt);
		break;
	}
	us = (uint32_t)(sys_time_us() - t0);
	prof.p.steps[kind]++;
	prof.p.kind_us[kind] += us;
	prof.p.us += us;
	prof.frame_us += us;
}

void phy_motion_prof(phy_motion_prof_t *p, int reset)
{
	*p = prof.p;
	if (prof.in_frame && prof.frame_us > p->us_max)
		p->us_max = prof.frame_us;
	p->traces = phy_trace_count;
	if (reset)
	{
		memset(&prof.p, 0, sizeof(prof.p));
		prof.frame_us = 0;
		prof.in_frame = 0;
		phy_trace_count = 0;
	}
}
