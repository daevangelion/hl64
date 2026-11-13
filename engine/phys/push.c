/* SPDX-License-Identifier: MIT */
/*
 * push.c -- MOVETYPE_PUSH (docs/specs/physics-movetypes.md 4,
 * think-and-time.md 4): the pusher's local clock and think, translation
 * and rotation with riders and the entities in its way, blocking.
 *
 * Candidates come from the spatial grid over the pusher's old and new
 * linked boxes (solids and triggers; riders always touch the old box), in
 * ascending index. Each is tested and pushed in its own layer's frame.
 * The player (slot 1) is a candidate like any other; its player block
 * (ground, flags, home-local origin) is read and written with it.
 */
#include <math.h>
#include <string.h>

#include "hl64/ent.h"
#include "hl64/rec.h"
#include "engine/core/hlmath.h"
#include "engine/core/log.h"
#include "engine/ent/ent.h"
#include "engine/phys/motion.h"
#include "engine/phys/phys.h"
#include "engine/phys/player.h"

/* entities one pusher moves in a frame; more are logged and left */
#define PUSH_MAX	64

static struct {
	uint16_t	ent[PUSH_MAX];
	float		org[PUSH_MAX][3];	/* where each was (its own frame) */
	float		yaw[PUSH_MAX];
	int		n;
	uint16_t	found[PUSH_MAX];
	uint8_t		warned;
} P;

static int nonzero(const float v[3])
{
	return v[0] != 0 || v[1] != 0 || v[2] != 0;
}

/* The movetype that decides whether c can be pushed (the player's view
 * carries none: its block holds it). */
static int pushable(int c)
{
	int mt;

	if (c == ENT_PLAYER)
		return phy_player_active() && phy_player()->movetype != PL_MOVE_NOCLIP;
	mt = ent_c.cv[c].movetype;
	return mt != MOVETYPE_PUSH && mt != MOVETYPE_NONE && mt != MOVETYPE_NOCLIP
		&& mt != MOVETYPE_FOLLOW;
}

static int rides(int c, int e)
{
	if (c == ENT_PLAYER)
	{
		const ent_player_t *pl = phy_player();

		return (pl->flags & PL_FL_ONGROUND) && pl->groundentity == e;
	}
	return (ent_c.cv[c].flags & FL_ONGROUND) && ent_c.phys[c].groundentity == e;
}

/* c's box inside the pusher alone (frame = c's layer). */
static int inside(int e, int c)
{
	const col_ent_t *cv = &ent_c.cv[c];
	col_trace_t tr;

	phy_trace_count++;
	col_clip(&ent_c.cv[e], cv->origin, cv->mins, cv->maxs, cv->origin, &tr);
	return tr.startsolid;
}

/* c's linked box overlaps the pusher's, faces in contact excluded
 * [probe:measured P-push down_onto_crate] (frame = c's layer). */
static int overlaps(int e, int c)
{
	const col_ent_t *p = &ent_c.cv[e], *v = &ent_c.cv[c];
	float d[3];
	int k;

	ent_layer_delta(ent_c.id[e].layer, ent_c.id[c].layer, d);
	for (k = 0; k < 3; k++)
		if (v->absmin[k] >= p->absmax[k] + d[k] || v->absmax[k] <= p->absmin[k] + d[k])
			return 0;
	return 1;
}

/* c's box at its origin is in something solid (the position test,
 * trace-and-hulls.md 4.6): blocked after its push [probe:measured P-push,
 * K-pusher]. */
static int stuck(int c)
{
	const col_ent_t *v = &ent_c.cv[c];
	col_trace_t tr;

	phy_trace(v->origin, v->mins, v->maxs, v->origin, phy_move_type(v), v, &tr);
	return tr.startsolid;
}

static void sync_player(int c)
{
	if (c == ENT_PLAYER && phy_player_active())
		memcpy(phy_player()->origin, ent_c.cv[c].origin, sizeof(ent_c.cv[c].origin));
}

/* Return every candidate moved so far (and their yaws). */
static void undo(int upto)
{
	int i;

	for (i = upto - 1; i >= 0; i--)
	{
		int c = P.ent[i], layer = eng_frame_layer();

		if (!ent_is_live(c))
			continue;
		memcpy(ent_c.cv[c].origin, P.org[i], sizeof(P.org[i]));
		if (c != ENT_PLAYER)
			ent_c.cv[c].angles[1] = P.yaw[i];
		sync_player(c);
		eng_set_frame_layer(ent_c.id[c].layer);
		phy_link_touch(&ent_c.cv[c]);
		eng_set_frame_layer(layer);
	}
}

/* Candidates: entities whose linked box meets [mins, maxs] (the pusher's
 * frame), ascending index, the pusher excluded. Pending and dry records
 * in the box hydrate first (architecture.md 6.4: moves wake the records
 * in their box): GoldSrc's pusher meets every map entity, near the
 * player or not (hldemo3's dish momentary_doors carry the func_pushable
 * through the `on` trigger 1300 u from the control room). */
static int gather(int e, const float mins[3], const float maxs[3])
{
	const float *gm = eng_layer_origin(eng_frame_layer());
	float gmins[3], gmaxs[3];
	int n, i, j = 0, k;

	for (k = 0; k < 3; k++)
	{
		gmins[k] = mins[k] + gm[k];
		gmaxs[k] = maxs[k] + gm[k];
	}
	rec_wake_box(gmins, gmaxs);
	n = ent_query_in(eng_frame_layer(), mins, maxs, ENT_Q_SOLID | ENT_Q_TRIGGER, P.found,
		PUSH_MAX);

	if (n == PUSH_MAX && !P.warned)
	{
		log_printf("phys: pusher %d (%s): %d candidates, extra left\n", e, ent_class_name(e), n);
		P.warned = 1;
	}
	for (i = 0; i < n; i++)
		if (P.found[i] != e && pushable(P.found[i]))
			P.found[j++] = P.found[i];
	return j;
}

/*
 * Move every candidate: riders, and entities inside the pusher at its new
 * place. rot: the rotation (forward / right / up of amove, pusher origin
 * and yaw change) or NULL for a translation by move. Returns the blocking
 * entity, or -1.
 */
typedef struct {
	float	f[3], r[3], u[3];
	float	yaw;
} turn_t;

static int carry(int e, int n, const float move[3], const turn_t *rot)
{
	col_ent_t *pv = &ent_c.cv[e];
	int frame = eng_frame_layer(), i, k;
	uint8_t solid = pv->solid;

	P.n = 0;
	for (i = 0; i < n; i++)
	{
		int c = P.found[i], blocked;
		col_ent_t *cv;
		uint16_t serial;
		float d[3], delta[3];
		col_trace_t tr;

		if (!ent_is_live(c))
			continue;
		cv = &ent_c.cv[c];
		serial = ent_c.id[c].serial;
		eng_set_frame_layer(ent_c.id[c].layer);
		if (!rides(c, e) && (!overlaps(e, c) || !inside(e, c)))
		{
			eng_set_frame_layer(frame);
			continue;
		}
		if (P.n == PUSH_MAX)
		{
			eng_set_frame_layer(frame);
			break;
		}
		P.ent[P.n] = (uint16_t)c;
		memcpy(P.org[P.n], cv->origin, sizeof(P.org[0]));
		P.yaw[P.n] = cv->angles[1];
		P.n++;
		if (c != ENT_PLAYER)
			cv->flags &= ~FL_ONGROUND;
		if (rot)
		{
			float rel[3];

			ent_layer_delta(ent_c.id[c].layer, frame, delta);
			for (k = 0; k < 3; k++)
				rel[k] = cv->origin[k] + delta[k] - pv->origin[k];
			for (k = 0; k < 3; k++)
				d[k] = rot->f[k] * rel[0] - rot->r[k] * rel[1] + rot->u[k] * rel[2]
					- rel[k];
			if (c != ENT_PLAYER)
				cv->angles[1] += rot->yaw;
		}
		else
			memcpy(d, move, sizeof(d));
		pv->solid = COL_SOLID_NOT;
		phy_shove(c, d, &tr);
		pv->solid = solid;
		if (!phy_alive(c, serial))
		{
			eng_set_frame_layer(frame);
			continue;
		}
		sync_player(c);
		ent_motion_changed(c);
		blocked = (cv->mins[0] != cv->maxs[0] || cv->mins[1] != cv->maxs[1]) && stuck(c);
		if (blocked && (cv->solid == COL_SOLID_NOT || cv->solid == COL_SOLID_TRIGGER))
		{
			/* corpses and gibs collapse instead of blocking
			 * [probe:measured P-push] */
			memset(cv->mins, 0, sizeof(cv->mins));
			memset(cv->maxs, 0, sizeof(cv->maxs));
			ent_link(cv);
			blocked = 0;
		}
		eng_set_frame_layer(frame);
		if (blocked)
			return c;
	}
	return -1;
}

/* Translation over mt (spec 4.2); 1 when not blocked. */
static int translate(int e, float mt, float ltime0)
{
	col_ent_t *pv = &ent_c.cv[e];
	ent_tm_t *tm = &ent_c.tm[e];
	const float *vel = ent_c.mot[e].velocity;
	float move[3], org[3], mins[3], maxs[3];
	int k, n, b;

	if (!nonzero(vel))
	{
		tm->ltime = ltime0 + mt;
		return 1;
	}
	memcpy(org, pv->origin, sizeof(org));
	memcpy(mins, pv->absmin, sizeof(mins));
	memcpy(maxs, pv->absmax, sizeof(maxs));
	for (k = 0; k < 3; k++)
	{
		move[k] = vel[k] * mt;
		pv->origin[k] += move[k];
	}
	tm->ltime = ltime0 + mt;
	ent_link(pv);
	if (pv->solid == COL_SOLID_NOT)
		return 1;
	for (k = 0; k < 3; k++)
	{
		if (pv->absmin[k] < mins[k])
			mins[k] = pv->absmin[k];
		if (pv->absmax[k] > maxs[k])
			maxs[k] = pv->absmax[k];
	}
	n = gather(e, mins, maxs);
	b = carry(e, n, move, NULL);
	if (b < 0)
		return 1;
	/* blocked: the blocker and every earlier candidate return, the
	 * pusher returns, its clock stalls; `blocked` once */
	undo(P.n);
	memcpy(pv->origin, org, sizeof(org));
	tm->ltime = ltime0;
	ent_link(pv);
	phy_blocked(e, b);
	return 0;
}

static void angle_basis(const float a[3], turn_t *t)
{
	const double rad = 3.14159265358979323846 / 180.0;
	double sp = hl_sin(a[0] * rad), cp = hl_cos(a[0] * rad);
	double sy = hl_sin(a[1] * rad), cy = hl_cos(a[1] * rad);
	double sr = hl_sin(a[2] * rad), cr = hl_cos(a[2] * rad);

	t->f[0] = (float)(cp * cy);
	t->f[1] = (float)(cp * sy);
	t->f[2] = (float)-sp;
	t->r[0] = (float)(-sr * sp * cy + cr * sy);
	t->r[1] = (float)(-sr * sp * sy - cr * cy);
	t->r[2] = (float)(-sr * cp);
	t->u[0] = (float)(cr * sp * cy + sr * sy);
	t->u[1] = (float)(cr * sp * sy - sr * cy);
	t->u[2] = (float)(cr * cp);
	t->yaw = a[1];
}

/* Rotation over mt (spec 4.3); 1 when not blocked. */
static int rotate(int e, float mt, float ltime0)
{
	col_ent_t *pv = &ent_c.cv[e];
	ent_tm_t *tm = &ent_c.tm[e];
	const float *av = ent_c.mot[e].avelocity;
	float amove[3], ang[3], mins[3], maxs[3];
	turn_t t;
	int k, n, b;

	memcpy(ang, pv->angles, sizeof(ang));
	memcpy(mins, pv->absmin, sizeof(mins));
	memcpy(maxs, pv->absmax, sizeof(maxs));
	for (k = 0; k < 3; k++)
	{
		amove[k] = av[k] * mt;
		pv->angles[k] += amove[k];
	}
	tm->ltime = ltime0 + mt;
	ent_link(pv);
	if (pv->solid == COL_SOLID_NOT)
		return 1;
	for (k = 0; k < 3; k++)
	{
		if (pv->absmin[k] < mins[k])
			mins[k] = pv->absmin[k];
		if (pv->absmax[k] > maxs[k])
			maxs[k] = pv->absmax[k];
	}
	angle_basis(amove, &t);
	n = gather(e, mins, maxs);
	b = carry(e, n, NULL, &t);
	if (b < 0)
		return 1;
	undo(P.n);
	memcpy(pv->angles, ang, sizeof(ang));
	tm->ltime = ltime0;
	ent_link(pv);
	phy_blocked(e, b);
	return 0;
}

void phy_pusher_step(int e, double time, double dt)
{
	ent_tm_t *tm = &ent_c.tm[e];
	const ent_mot_t *m = &ent_c.mot[e];
	float ltime0 = tm->ltime, nt = tm->nextthink, mt;

	if ((double)nt < (double)ltime0 + dt)
	{
		mt = nt - ltime0;
		if (mt < 0)
			mt = 0;
	}
	else
		mt = (float)dt;
	if (mt > 0)
	{
		if (nonzero(m->avelocity))
		{
			if (rotate(e, mt, ltime0) && nonzero(m->velocity))
				translate(e, mt, ltime0);
		}
		else
			translate(e, mt, ltime0);
	}
	if (nt > ltime0 && nt <= tm->ltime)
		phy_call_think(e, time);
}
