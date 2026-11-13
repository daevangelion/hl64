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
 * pm_host.c -- the pm_shared adapter (docs/specs/pmove-host.md): copies
 * the engine's move record (include/hl64/pmove.h) into HLSDK's
 * playermove_t, wires the playermove_t services to the engine's
 * (contents, player traces, hulls, materials, sounds, random), runs
 * PM_Move as the server and writes the player block and the touch list
 * back. Physent indices are the record's; touch entries carry them.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "hl64/game.h"
#include "hl64/settings.h"

#include "mathlib.h"
#include "const.h"
#include "usercmd.h"
#include "pm_defs.h"
#include "pm_shared.h"
#include "pm_movevars.h"

qboolean PM_AddToTouched(pmtrace_t tr, vec3_t impactvelocity);

/* the player block's physics info string is copied whole */
typedef char pm_physinfo_size[MAX_PHYSINFO_STRING == HL64_PM_PHYSINFO ? 1 : -1];

/* A physent's model pointer: the record entry it came from. */
struct model_s
{
	const col_ent_t	*e;
};

static playermove_t	pm;
static movevars_t	mv;
static struct model_s	pe_model[MAX_PHYSENTS], me_model[MAX_MOVEENTS];
static col_hull_t	hulls[2];
static hull_t		hull_handle[2];
static int		hull_next;

/* usehull -> clip hull (spec 3: 0 -> 1, 1 -> 3, 2 -> 0, 3 -> 2) */
static const int	usehull_hull[4] = { 1, 3, 0, 2 };

static const col_ent_t *pe_src(const physent_t *pe)
{
	if (pe >= pm.physents && pe < pm.physents + pm.numphysent)
		return &phy_pm.pe[pe - pm.physents];
	if (pe >= pm.moveents && pe < pm.moveents + pm.nummoveent)
		return &phy_pm.me[pe - pm.moveents];
	return NULL;
}

static void to_pmtrace(const col_trace_t *t, pmtrace_t *o)
{
	o->allsolid = t->allsolid ? true : false;
	o->startsolid = t->startsolid ? true : false;
	o->inopen = t->inopen ? true : false;
	o->inwater = t->inwater ? true : false;
	o->fraction = t->frac;
	VectorCopy(t->end, o->endpos);
	VectorCopy(t->normal, o->plane.normal);
	o->plane.dist = t->dist;
	o->ent = t->ent;
	VectorClear(o->deltavelocity);
	o->hitgroup = t->hitgroup;
}

/* ---- services ------------------------------------------------------------- */

static int svc_test_position(float *pos, pmtrace_t *ptrace)
{
	col_trace_t t;
	int hit = phy_pm_test(pos, pm.usehull, &t);

	if (ptrace)
		to_pmtrace(&t, ptrace);
	return hit;
}

static void svc_print(char *fmt, ...)
{
	char buf[128];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	phy_pm_print(buf);
}

static double svc_time(void)
{
	return phy_pm.time_ms * 0.001;
}

static void svc_stuck_touch(int hitent, pmtrace_t *ptraceresult)
{
	ptraceresult->ent = hitent;
	PM_AddToTouched(*ptraceresult, pm.velocity);
}

static int svc_point_contents(float *p, int *truecontents)
{
	return phy_pm_contents(p, truecontents);
}

static int svc_true_point_contents(float *p)
{
	return phy_pm_true_contents(p);
}

static int svc_hull_point_contents(struct hull_s *hull, int num, float *p)
{
	return col_hull_contents(&hulls[hull->ref], num, p);
}

static pmtrace_t svc_player_trace(float *start, float *end, int traceFlags, int ignore_pe)
{
	col_trace_t t;
	pmtrace_t o;

	phy_pm_trace(start, end, traceFlags, pm.usehull, ignore_pe, &t);
	to_pmtrace(&t, &o);
	return o;
}

static int svc_random_long(int lo, int hi)
{
	return hl64_rand_long(lo, hi);
}

static float svc_random_float(float lo, float hi)
{
	return hl64_rand_float(lo, hi);
}

/* only brush physents carry a model: mod_brush */
static int svc_model_type(struct model_s *mod)
{
	(void)mod;
	return 0;
}

/* GoldSrc returns model-space bounds, which for a map's brush model are
 * map coordinates; the move runs in global coordinates, so the engine
 * adds the model's layer placement (PM_LadderMove aims at the centre) */
static void svc_model_bounds(struct model_s *mod, float *mins, float *maxs)
{
	if (!phy_pm_model_bounds(mod->e->model, mins, maxs))
	{
		VectorClear(mins);
		VectorClear(maxs);
	}
}

static void *svc_hull_for_bsp(physent_t *pe, float *offset)
{
	const col_ent_t *e = pe_src(pe);
	int i = hull_next;

	hull_next ^= 1;
	hull_handle[i].ref = i;
	hull_handle[i].firstclipnode = 0;
	VectorClear(offset);
	if (e && e->model != COL_NONE &&
		col_hull_get(e->model, usehull_hull[pm.usehull], e->origin, &hulls[i], offset))
		hull_handle[i].firstclipnode = hulls[i].root;
	else
	{
		/* no model: an empty hull (a leaf child that reads EMPTY) */
		memset(&hulls[i], 0, sizeof(hulls[i]));
		hulls[i].model = COL_NONE;
		hulls[i].root = CONTENTS_EMPTY;
		hull_handle[i].firstclipnode = CONTENTS_EMPTY;
	}
	return &hull_handle[i];
}

/* GoldSrc's PM_TraceModel traces the point (hull 0) whatever usehull is:
 * PM_LadderMove's trace from inside the ladder's player hull toward its
 * centre then crosses the brush face [probe:measured PM-bot ladder runs] */
static float svc_trace_model(physent_t *pEnt, float *start, float *end, trace_t *trace)
{
	const col_ent_t *e = pe_src(pEnt);
	col_trace_t t;

	memset(trace, 0, sizeof(*trace));
	if (!e)
	{
		trace->fraction = 1;
		VectorCopy(end, trace->endpos);
		return 1;
	}
	phy_pm_trace_ent(e, 2, start, end, &t);
	trace->allsolid = t.allsolid ? true : false;
	trace->startsolid = t.startsolid ? true : false;
	trace->inopen = t.inopen ? true : false;
	trace->inwater = t.inwater ? true : false;
	trace->fraction = t.frac;
	VectorCopy(t.end, trace->endpos);
	VectorCopy(t.normal, trace->plane.normal);
	trace->plane.dist = t.dist;
	trace->ent = (int)(pEnt - pm.physents);
	trace->hitgroup = t.hitgroup;
	return t.frac;
}

static void svc_play_sound(int channel, const char *sample, float volume, float attenuation,
	int fFlags, int pitch)
{
	phy_pm_sound(channel, sample, volume, attenuation, fFlags, pitch);
}

/* the texture-type character under the player on the ground physent */
static char svc_trace_texture(int ground, float *vstart, float *vend)
{
	const col_ent_t *e;

	if (ground < 0 || ground >= pm.numphysent)
		return 0;
	e = &phy_pm.pe[ground];
	if (e->model == COL_NONE)
		return 0;
	return (char)col_material(e->model, e->origin, e->angles, vstart, vend);
}

/* ---- the move ------------------------------------------------------------- */

void game_pm_init(void)
{
	int i;

	memset(&pm, 0, sizeof(pm));
	for (i = 0; i < 4; i++)
	{
		VectorCopy(phy_pm_mins[i], pm.player_mins[i]);
		VectorCopy(phy_pm_maxs[i], pm.player_maxs[i]);
	}
	pm.movevars = &mv;
	pm.PM_Info_ValueForKey = phy_pm_info_value;
	pm.PM_TestPlayerPosition = svc_test_position;
	pm.Con_DPrintf = svc_print;
	pm.Con_Printf = svc_print;
	pm.Sys_FloatTime = svc_time;
	pm.PM_StuckTouch = svc_stuck_touch;
	pm.PM_PointContents = svc_point_contents;
	pm.PM_TruePointContents = svc_true_point_contents;
	pm.PM_HullPointContents = svc_hull_point_contents;
	pm.PM_PlayerTrace = svc_player_trace;
	pm.RandomLong = svc_random_long;
	pm.RandomFloat = svc_random_float;
	pm.PM_GetModelType = svc_model_type;
	pm.PM_GetModelBounds = svc_model_bounds;
	pm.PM_HullForBsp = svc_hull_for_bsp;
	pm.PM_TraceModel = svc_trace_model;
	pm.PM_PlaySound = svc_play_sound;
	pm.PM_TraceTexture = svc_trace_texture;
	PM_Init(&pm);
}

static void physent_in(physent_t *o, const col_ent_t *e, struct model_s *m)
{
	o->player = 0;
	VectorCopy(e->origin, o->origin);
	VectorCopy(e->angles, o->angles);
	VectorCopy(e->mins, o->mins);
	VectorCopy(e->maxs, o->maxs);
	o->info = e->index;
	o->solid = e->solid;
	o->skin = e->skin;
	o->rendermode = e->rendermode;
	m->e = e;
	o->model = e->model != COL_NONE ? m : NULL;
}

/* physent index of the player's ground entity, -1 none */
static int ground_physent(const ent_player_t *pl)
{
	int i;

	if (!(pl->flags & FL_ONGROUND) || pl->groundentity < 0)
		return -1;
	for (i = 0; i < phy_pm.npe; i++)
		if (phy_pm.pe[i].index == (uint16_t)pl->groundentity)
			return i;
	return -1;
}

void game_pm_move(phy_pm_t *rec)
{
	ent_player_t *pl = rec->pl;
	int i;

	/* movevars (pmove-host.md 4) */
	mv.gravity = settings.gravity;
	mv.stopspeed = settings.stopspeed;
	mv.maxspeed = settings.maxspeed;
	mv.spectatormaxspeed = settings.spectatormaxspeed;
	mv.accelerate = settings.accelerate;
	mv.airaccelerate = settings.airaccelerate;
	mv.wateraccelerate = settings.wateraccelerate;
	mv.friction = settings.friction;
	mv.edgefriction = settings.edgefriction;
	mv.waterfriction = settings.waterfriction;
	mv.entgravity = settings.entgravity;
	mv.bounce = settings.bounce;
	mv.stepsize = settings.stepsize;
	mv.maxvelocity = settings.maxvelocity;
	mv.zmax = settings.zmax;
	mv.waveHeight = settings.waveheight;
	mv.footsteps = settings.footsteps ? true : false;
	mv.rollangle = settings.rollangle;
	mv.rollspeed = settings.rollspeed;

	/* world state */
	pm.numphysent = rec->npe;
	for (i = 0; i < rec->npe; i++)
		physent_in(&pm.physents[i], &rec->pe[i], &pe_model[i]);
	pm.nummoveent = rec->nme;
	for (i = 0; i < rec->nme; i++)
		physent_in(&pm.moveents[i], &rec->me[i], &me_model[i]);

	/* player state (spec 1 step 6 lists the fields that come back) */
	pm.player_index = 0;
	pm.multiplayer = false;
	pm.runfuncs = true;
	pm.time = rec->time_ms;
	VectorCopy(pl->origin, pm.origin);
	VectorCopy(pl->angles, pm.angles);
	VectorCopy(pl->angles, pm.oldangles);
	VectorCopy(pl->velocity, pm.velocity);
	VectorCopy(pl->movedir, pm.movedir);
	VectorCopy(pl->basevelocity, pm.basevelocity);
	VectorCopy(pl->view_ofs, pm.view_ofs);
	pm.flDuckTime = pl->duck_time;
	pm.bInDuck = pl->in_duck ? true : false;
	pm.flTimeStepSound = pl->step_time;
	pm.iStepLeft = pl->step_left;
	pm.flFallVelocity = pl->fall_velocity;
	VectorCopy(pl->punchangle, pm.punchangle);
	pm.flSwimTime = pl->swim_time;
	pm.flags = (int)pl->flags;
	pm.usehull = pl->usehull;
	pm.gravity = pl->gravity;
	pm.friction = pl->friction;
	pm.oldbuttons = pl->buttons;
	pm.waterjumptime = pl->waterjump_time;
	pm.dead = pl->dead ? true : false;
	pm.deadflag = pl->dead ? DEAD_DEAD : DEAD_NO;
	pm.spectator = 0;
	pm.iuser1 = pm.iuser2 = pm.iuser3 = 0;
	pm.movetype = pl->movetype;
	pm.onground = ground_physent(pl);
	pm.waterlevel = pl->waterlevel;
	pm.watertype = pl->watertype;
	pm.maxspeed = settings.maxspeed;
	pm.clientmaxspeed = pl->maxspeed;
	memcpy(pm.physinfo, pl->physinfo, sizeof(pm.physinfo));

	memset(&pm.cmd, 0, sizeof(pm.cmd));
	pm.cmd.msec = rec->cmd.msec;
	VectorCopy(rec->cmd.viewangles, pm.cmd.viewangles);
	pm.cmd.forwardmove = rec->cmd.forwardmove;
	pm.cmd.sidemove = rec->cmd.sidemove;
	pm.cmd.upmove = rec->cmd.upmove;
	pm.cmd.buttons = rec->cmd.buttons;
	pm.cmd.impulse = rec->cmd.impulse;

	PM_Move(&pm, 1);

	/* write back */
	VectorCopy(pm.origin, pl->origin);
	VectorCopy(pm.angles, pl->angles);
	VectorCopy(rec->cmd.viewangles, pl->view_angles);
	VectorCopy(pm.velocity, pl->velocity);
	VectorCopy(pm.movedir, pl->movedir);
	VectorCopy(pm.basevelocity, pl->basevelocity);
	VectorCopy(pm.view_ofs, pl->view_ofs);
	pl->duck_time = pm.flDuckTime;
	pl->in_duck = pm.bInDuck ? 1 : 0;
	pl->step_time = pm.flTimeStepSound;
	pl->step_left = (uint8_t)pm.iStepLeft;
	pl->fall_velocity = pm.flFallVelocity;
	VectorCopy(pm.punchangle, pl->punchangle);
	pl->swim_time = pm.flSwimTime;
	pl->flags = (uint32_t)pm.flags;
	pl->usehull = (uint8_t)pm.usehull;
	VectorCopy(phy_pm_mins[pm.usehull], pl->mins);
	VectorCopy(phy_pm_maxs[pm.usehull], pl->maxs);
	pl->friction = pm.friction;
	pl->oldbuttons = (uint16_t)pm.oldbuttons;
	pl->buttons = rec->cmd.buttons;
	pl->waterjump_time = pm.waterjumptime;
	pl->movetype = (uint8_t)pm.movetype;
	pl->waterlevel = (int8_t)pm.waterlevel;
	pl->watertype = (int8_t)pm.watertype;
	/* the ground entity stays while airborne (FL_ONGROUND tells)
	 * [probe:measured PM-bot] */
	if (pm.onground >= 0 && pm.onground < pm.numphysent)
		pl->groundentity = (int16_t)pm.physents[pm.onground].info;

	rec->ntouch = pm.numtouch;
	for (i = 0; i < pm.numtouch; i++)
	{
		const pmtrace_t *t = &pm.touchindex[i];
		col_trace_t *o = &rec->touch[i];

		o->frac = t->fraction;
		VectorCopy(t->endpos, o->end);
		VectorCopy(t->plane.normal, o->normal);
		o->dist = t->plane.dist;
		o->ent = (int16_t)t->ent;
		o->allsolid = t->allsolid ? 1 : 0;
		o->startsolid = t->startsolid ? 1 : 0;
		o->inopen = t->inopen ? 1 : 0;
		o->inwater = t->inwater ? 1 : 0;
		o->hitgroup = (uint8_t)t->hitgroup;
		o->pad = 0;
	}
}
