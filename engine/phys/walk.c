/* SPDX-License-Identifier: MIT */
/*
 * walk.c -- the engine's monster movement services
 * (docs/specs/physics-movetypes.md 6; include/hl64/ent.h): walk_move,
 * move_to_origin, drop_to_floor, change_yaw, check_bottom and the
 * blocking entity of the last step trace (HLSDK gpGlobals->trace_ent).
 * Positions are map-local to the caller's frame (eng_frame_layer()),
 * which the engine sets to the monster's layer around its think.
 */
#include <math.h>
#include <string.h>

#include "hl64/ent.h"
#include "hl64/pmove.h"
#include "hl64/settings.h"
#include "engine/ent/ent.h"
#include "engine/phys/motion.h"

/* Game side (ent.h); none linked: no enemy. */
#pragma weak game_walk_enemy

#define DEG2RAD		(3.14159265358979323846 / 180.0)

static int trace_ent = -1;
static const float zero3[3];

int phy_trace_ent(void)
{
	return trace_ent;
}

static void move_trace(const col_ent_t *v, const float s[3], const float e[3], int mode,
	col_trace_t *tr)
{
	phy_trace(s, v->mins, v->maxs, e, mode == WALKMOVE_WORLDONLY ? COL_MOVE_NOMONSTERS
		: COL_MOVE_NORMAL, v, tr);
	trace_ent = tr->ent;
}

int phy_check_bottom(int e)
{
	const col_ent_t *v = &ent_c.cv[e];
	float lo[3], hi[3], p[3], q[3], centre;
	col_trace_t tr;
	int x, y, k;

	for (k = 0; k < 3; k++)
	{
		lo[k] = v->origin[k] + v->mins[k];
		hi[k] = v->origin[k] + v->maxs[k];
	}
	/* all four corners just under the box in solid world: on ground */
	p[2] = lo[2] - 1;
	for (x = 0; x < 2; x++)
		for (y = 0; y < 2; y++)
		{
			p[0] = x ? hi[0] : lo[0];
			p[1] = y ? hi[1] : lo[1];
			if (col_contents(p) != COL_SOLID)
				goto traced;
		}
	return 1;
traced:
	/* the centre line must reach ground within two steps, and no corner
	 * may drop more than a step below the centre's ground */
	p[0] = (lo[0] + hi[0]) * 0.5f;
	p[1] = (lo[1] + hi[1]) * 0.5f;
	p[2] = lo[2];
	q[0] = p[0];
	q[1] = p[1];
	q[2] = p[2] - 2 * settings.stepsize;
	phy_trace(p, zero3, zero3, q, COL_MOVE_NOMONSTERS, v, &tr);
	if (tr.frac == 1)
		return 0;
	centre = tr.end[2];
	for (x = 0; x < 2; x++)
		for (y = 0; y < 2; y++)
		{
			p[0] = q[0] = x ? hi[0] : lo[0];
			p[1] = q[1] = y ? hi[1] : lo[1];
			phy_trace(p, zero3, zero3, q, COL_MOVE_NOMONSTERS, v, &tr);
			if (tr.frac == 1 || centre - tr.end[2] > settings.stepsize)
				return 0;
		}
	return 1;
}

/* The fly / swim branch: with an enemy, first try bending toward its
 * height, then the level move. */
static int move_flying(int e, const float d[3], int mode)
{
	col_ent_t *v = &ent_c.cv[e];
	int enemy = game_walk_enemy ? game_walk_enemy(e) : -1, pass, k;

	if (enemy >= 0 && !ent_is_live(enemy))
		enemy = -1;
	for (pass = 0; pass < 2; pass++)
	{
		float to[3];
		col_trace_t tr;

		for (k = 0; k < 3; k++)
			to[k] = v->origin[k] + d[k];
		if (pass == 0 && enemy >= 0)
		{
			float ed[3], dz;

			ent_frame_delta(enemy, ed);
			dz = v->origin[2] - (ent_c.cv[enemy].origin[2] + ed[2]);
			if (dz > 40)
				to[2] -= 8;
			if (dz < 30)
				to[2] += 8;
		}
		move_trace(v, v->origin, to, mode, &tr);
		if (tr.frac == 1)
		{
			if ((v->flags & FL_SWIM) && col_contents(tr.end) == COL_EMPTY)
				return 0;	/* swimmers stay in liquid */
			memcpy(v->origin, tr.end, sizeof(v->origin));
			if (mode != WALKMOVE_CHECKONLY)
				phy_link_touch(v);
			return 1;
		}
		if (enemy < 0)
			break;
	}
	return 0;
}

/* One step by d: up a step, down two (spec 6). */
static int move_step(int e, const float d[3], int mode)
{
	col_ent_t *v = &ent_c.cv[e];
	float old[3], from[3], to[3];
	col_trace_t tr;
	int k;

	if (v->flags & (FL_FLY | FL_SWIM))
		return move_flying(e, d, mode);
	memcpy(old, v->origin, sizeof(old));
	for (k = 0; k < 3; k++)
		from[k] = to[k] = v->origin[k] + d[k];
	from[2] += settings.stepsize;
	to[2] -= settings.stepsize;
	move_trace(v, from, to, mode, &tr);
	if (tr.allsolid)
		return 0;
	if (tr.startsolid)
	{
		from[2] -= settings.stepsize;
		move_trace(v, from, to, mode, &tr);
		if (tr.allsolid || tr.startsolid)
			return 0;
	}
	if (tr.frac == 1)
	{
		/* nothing under: only a partly supported monster falls */
		if (!(v->flags & FL_PARTIALGROUND))
			return 0;
		for (k = 0; k < 3; k++)
			v->origin[k] += d[k];
		if (mode != WALKMOVE_CHECKONLY)
		{
			phy_link_touch(v);
			v->flags &= ~FL_ONGROUND;
			ent_motion_changed(e);
		}
		return 1;
	}
	memcpy(v->origin, tr.end, sizeof(v->origin));
	if (!phy_check_bottom(e))
	{
		if (v->flags & FL_PARTIALGROUND)
		{
			if (mode != WALKMOVE_CHECKONLY)
				phy_link_touch(v);
			return 1;
		}
		memcpy(v->origin, old, sizeof(old));
		return 0;
	}
	if (mode == WALKMOVE_CHECKONLY)
		return 1;
	v->flags &= ~FL_PARTIALGROUND;
	ent_c.phys[e].groundentity = (uint16_t)(tr.ent < 0 ? 0 : tr.ent);
	phy_link_touch(v);
	ent_motion_changed(e);
	return 1;
}

int phy_walk_move(int e, float yaw, float dist, int mode)
{
	float d[3];
	double a = yaw * DEG2RAD;

	if (!(ent_c.cv[e].flags & (FL_ONGROUND | FL_FLY | FL_SWIM)))
		return 0;
	d[0] = (float)(cos(a) * dist);
	d[1] = (float)(sin(a) * dist);
	d[2] = 0;
	return move_step(e, d, mode);
}

/*
 * move_to_origin (spec 6): straight at the goal; when that is blocked, the
 * eight compass directions in order of how much nearer each brings the
 * goal (ties in a random order from the engine stream), the reverse of
 * the straight direction last.
 */
int phy_move_to_origin(int e, const float goal[3], float dist, int mode)
{
	col_ent_t *v = &ent_c.cv[e];
	float dx = goal[0] - v->origin[0], dy = goal[1] - v->origin[1], gain[8];
	int order[8], n = 0, i, j, back;
	double yaw;

	if (!(v->flags & (FL_ONGROUND | FL_FLY | FL_SWIM)))
		return 0;
	yaw = atan2(dy, dx) / DEG2RAD;
	if (phy_walk_move(e, (float)yaw, dist, mode))
		return 1;
	back = (int)floor((yaw + 180.0) / 45.0 + 0.5) & 7;
	for (i = 0; i < 8; i++)
	{
		double a = i * 45 * DEG2RAD;

		if (i == back)
			continue;
		gain[i] = (float)(cos(a) * dx + sin(a) * dy);
		/* insertion by gain, a random side on ties */
		for (j = n; j > 0; j--)
		{
			float g = gain[order[j - 1]];

			if (g > gain[i] || (g == gain[i] && hl64_rand_long(0, 1)))
				break;
			order[j] = order[j - 1];
		}
		order[j] = i;
		n++;
	}
	order[n++] = back;
	for (i = 0; i < n; i++)
		if (phy_walk_move(e, (float)(order[i] * 45), dist, mode))
			return 1;
	return 0;
}

int phy_drop_to_floor(int e)
{
	col_ent_t *v = &ent_c.cv[e];
	float end[3];
	col_trace_t tr;

	end[0] = v->origin[0];
	end[1] = v->origin[1];
	end[2] = v->origin[2] - 256;
	phy_trace(v->origin, v->mins, v->maxs, end, COL_MOVE_NORMAL, v, &tr);
	if (tr.allsolid)
		return -1;
	if (tr.frac == 1)
		return 0;
	memcpy(v->origin, tr.end, sizeof(v->origin));
	ent_link(v);
	v->flags |= FL_ONGROUND;
	ent_c.phys[e].groundentity = (uint16_t)(tr.ent < 0 ? 0 : tr.ent);
	ent_motion_changed(e);
	return 1;
}

static float wrap360(float a)
{
	a = (float)fmod(a, 360.0);
	if (a < 0)
		a += 360;
	return a;
}

void phy_change_yaw(int e)
{
	float *ang = ent_c.cv[e].angles, cur = wrap360(ang[1]);
	float ideal = ent_c.phys[e].ideal_yaw, speed = ent_c.phys[e].yaw_speed, turn;

	if (cur == ideal)
		return;
	turn = ideal - cur;
	if (turn >= 180)
		turn -= 360;
	else if (turn < -180)
		turn += 360;
	if (turn > speed)
		turn = speed;
	else if (turn < -speed)
		turn = -speed;
	ang[1] = wrap360(cur + turn);
}
