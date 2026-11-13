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
 * grenade.c -- CGrenade (HLSDK dlls/ggrenade.cpp): Explode (TE_EXPLOSION
 * through fx_explosion, radius damage, scorch decal, debris sound, spark
 * showers), Smoke, Detonate, ExplodeTouch, BounceTouch, TumbleThink,
 * DangerSoundThink, ShootTimed / ShootContact. MOVETYPE_BOUNCE with
 * gravity 0.5 (and friction 0.8 for hand grenades) as HLSDK sets them;
 * the engine's bounce movetype does the rest.
 */
#include "game/hl/grenade.h"
#include "game/hl/player.h"
#include "game/hl/util.h"

#define NORMAL_EXPLOSION_VOLUME	1024
#define TE_EXPLFLAG_NONE	0

#define G(e)	PRIV(CGrenade, e)

void CGrenade_Smoke(int e)
{
	CGrenade *g = G(e);
	const float *o = ent_cv(e)->origin;

	if (UTIL_PointContents(o) == CONTENTS_WATER)
		;	/* UTIL_Bubbles: no bubble effect in the effects API */
	else
		fx_smoke(o, RESID(SPR_SMOKE), (int)((g->dmg - 50) * 0.80f), 12);
	UTIL_Remove(e);
}

void CGrenade_Explode(int e, const col_trace_t *tr, int bits)
{
	CGrenade *g = G(e);
	col_ent_t *c = ent_cv(e);
	float org[3];
	int contents, owner, i, n;

	c->model = COL_NONE;
	ent_rnd(e)->model = RES_NONE;	/* invisible */
	ent_set_solid(e, SOLID_NOT);	/* intangible */
	g->takedamage = DAMAGE_NO;
	/* pull out of the wall a bit */
	VecCopy(c->origin, org);
	if (tr->frac != 1.0f)
	{
		VecMA(tr->end, (g->dmg - 24) * 0.6f, tr->normal, org);
		UTIL_SetOrigin(e, org);
	}
	contents = UTIL_PointContents(org);
	fx_explosion(org, contents != CONTENTS_WATER ? RESID(SPR_FIREBALL) : RESID(SPR_WXPLO1),
		(int)((g->dmg - 50) * 0.60f), 15, TE_EXPLFLAG_NONE);
	CSoundEnt_InsertSound(bits_SOUND_COMBAT, org, NORMAL_EXPLOSION_VOLUME, 3.0f);
	owner = c->owner != COL_NONE ? c->owner : -1;
	c->owner = COL_NONE;	/* can't traceline attack owner if this is set */
	RadiusDamage(org, e, owner, g->dmg, g->dmg * 2.5f, CLASS_NONE, bits);
	UTIL_DecalTrace(tr, RANDOM_FLOAT(0, 1) < 0.5f ? DECAL_SCORCH1 : DECAL_SCORCH2);
	switch (RANDOM_LONG(0, 2))
	{
	case 0: EMIT_SOUND(e, CHAN_VOICE, RESID(W_DEBRIS1), 0.55f, ATTN_NORM); break;
	case 1: EMIT_SOUND(e, CHAN_VOICE, RESID(W_DEBRIS2), 0.55f, ATTN_NORM); break;
	case 2: EMIT_SOUND(e, CHAN_VOICE, RESID(W_DEBRIS3), 0.55f, ATTN_NORM); break;
	}
	ent_rnd(e)->effects |= EF_NODRAW;
	SetThink(e, FN_CGrenade_Smoke);
	ent_set_velocity(e, g_vecZero);
	ThinkIn(e, 0.3f);
	if (contents != CONTENTS_WATER)
	{
		n = RANDOM_LONG(0, 3);
		for (i = 0; i < n; i++)
			CBaseEntity_Create(CLS_SPARK_SHOWER, org, tr->normal, -1);
	}
}

void CGrenade_Detonate(int e)
{
	col_trace_t tr;
	float spot[3], end[3];

	VecCopy(ent_cv(e)->origin, spot);
	spot[2] += 8;
	VecCopy(spot, end);
	end[2] -= 40;
	UTIL_TraceLine(spot, end, ignore_monsters, e, &tr);
	CGrenade_Explode(e, &tr, DMG_BLAST);
}

void CGrenade_KilledVt(int e, int attacker, int gib)
{
	(void)attacker; (void)gib;
	CGrenade_Detonate(e);
}

void CGrenade_DetonateUse(int e, int activator, int caller, int use_type, float value)
{
	(void)activator; (void)caller; (void)use_type; (void)value;
	SetThink(e, FN_CGrenade_Detonate);
	ent_think_at(e, gtime());
}

void CGrenade_PreDetonate(int e)
{
	CSoundEnt_InsertSound(bits_SOUND_DANGER, ent_cv(e)->origin, 400, 0.3f);
	SetThink(e, FN_CGrenade_Detonate);
	ThinkIn(e, 1);
}

void CGrenade_ExplodeTouch(int e, int other)
{
	CGrenade *g = G(e);
	col_trace_t tr;
	float dir[3], spot[3], end[3];

	g->enemy = EHANDLE(other);
	VecNorm(ent_mot(e)->velocity, dir);
	VecMA(ent_cv(e)->origin, -32, dir, spot);
	VecMA(spot, 64, dir, end);
	UTIL_TraceLine(spot, end, ignore_monsters, e, &tr);
	CGrenade_Explode(e, &tr, DMG_BLAST);
}

void CGrenade_DangerSoundThink(int e)
{
	const float *v = ent_mot(e)->velocity;
	float p[3];

	if (!IsInWorld(e))
	{
		UTIL_Remove(e);
		return;
	}
	VecMA(ent_cv(e)->origin, 0.5f, v, p);
	CSoundEnt_InsertSound(bits_SOUND_DANGER, p, (int)VecLen(v), 0.2f);
	ThinkIn(e, 0.2f);
	if (ent_phys(e)->waterlevel != 0)
	{
		float h[3];

		VecScale(v, 0.5f, h);
		ent_set_velocity(e, h);
	}
}

static void BounceSound(int e)
{
	switch (RANDOM_LONG(0, 2))
	{
	case 0: EMIT_SOUND(e, CHAN_VOICE, RESID(W_GRENADE_HIT1), 0.25f, ATTN_NORM); break;
	case 1: EMIT_SOUND(e, CHAN_VOICE, RESID(W_GRENADE_HIT2), 0.25f, ATTN_NORM); break;
	case 2: EMIT_SOUND(e, CHAN_VOICE, RESID(W_GRENADE_HIT3), 0.25f, ATTN_NORM); break;
	}
}

void CGrenade_BounceTouch(int e, int other)
{
	CGrenade *g = G(e);
	ent_rnd_t *r = ent_rnd(e);
	float v[3], test[3];

	/* don't hit the guy that launched this grenade */
	if (ent_cv(e)->owner != COL_NONE && other == ent_cv(e)->owner)
		return;
	VecCopy(ent_mot(e)->velocity, v);
	/* only do damage if we're moving fairly fast */
	if (g->m_flNextAttack < gtime() && VecLen(v) > 100)
	{
		int owner = ent_cv(e)->owner != COL_NONE ? ent_cv(e)->owner : -1;

		if (owner >= 0)
		{
			/* UTIL_GetGlobalTrace: the impact of this move */
			col_trace_t tr = { 0 };

			tr.frac = 1;
			tr.ent = (int16_t)other;
			VecCopy(ent_cv(e)->origin, tr.end);
			ClearMultiDamage();
			TraceAttack(other, owner, 1, gpGlobals.v_forward, &tr, DMG_CLUB);
			ApplyMultiDamage(e, owner);
		}
		g->m_flNextAttack = gtime() + 1.0f;	/* debounce */
	}
	/* this is my heuristic for modulating the grenade velocity because
	 * grenades dropped purely vertical or thrown very far tend to slow
	 * down too quickly for me to always catch just by testing velocity */
	VecCopy(v, test);
	test[2] *= 0.45f;
	if (!g->m_fRegisteredSound && VecLen(test) <= 60)
	{
		/* grenade is moving really slow: it's probably very close to where
		 * it will ultimately stop moving; time to warn monsters */
		CSoundEnt_InsertSound(bits_SOUND_DANGER, ent_cv(e)->origin, (int)(g->dmg / 0.4f), 0.3f);
		g->m_fRegisteredSound = 1;
	}
	if (ent_cv(e)->flags & FL_ONGROUND)
	{
		/* add a bit of static friction */
		VecScale(v, 0.8f, v);
		ent_set_velocity(e, v);
		r->sequence = (uint16_t)RANDOM_LONG(1, 1);
	}
	else
		/* play bounce sound */
		BounceSound(e);
	r->framerate = VecLen(v) / 200.0f;
	if (r->framerate > 1.0f)
		r->framerate = 1;
	else if (r->framerate < 0.5f)
		r->framerate = 0;
}

void CGrenade_SlideTouch(int e, int other)
{
	/* don't hit the guy that launched this grenade */
	if (ent_cv(e)->owner != COL_NONE && other == ent_cv(e)->owner)
		return;
	if (ent_cv(e)->flags & FL_ONGROUND)
	{
		float v[3];

		/* add a bit of static friction */
		VecScale(ent_mot(e)->velocity, 0.95f, v);
		ent_set_velocity(e, v);
	}
	else
		BounceSound(e);
}

void CGrenade_TumbleThink(int e)
{
	CGrenade *g = G(e);

	if (!IsInWorld(e))
	{
		UTIL_Remove(e);
		return;
	}
	gs_frame_advance(e, &g->anim, 0);
	ThinkIn(e, 0.1f);
	if (g->dmgtime - 1 < gtime())
	{
		float p[3];

		VecMA(ent_cv(e)->origin, g->dmgtime - gtime(), ent_mot(e)->velocity, p);
		CSoundEnt_InsertSound(bits_SOUND_DANGER, p, 400, 0.1f);
	}
	if (g->dmgtime <= gtime())
		SetThink(e, FN_CGrenade_Detonate);
	if (ent_phys(e)->waterlevel != 0)
	{
		float v[3];

		VecScale(ent_mot(e)->velocity, 0.5f, v);
		ent_set_velocity(e, v);
		ent_rnd(e)->framerate = 0.2f;
	}
}

/* CGrenade::Spawn */
static void grenade_spawn(int e)
{
	CGrenade *g = G(e);

	ent_set_movetype(e, MOVETYPE_BOUNCE);
	ent_set_solid(e, SOLID_BBOX);
	ent_set_model(e, RESID(MDL_GRENADE));
	UTIL_SetSize(e, g_vecZero, g_vecZero);
	g->dmg = 100;
	g->m_fRegisteredSound = 0;
}

static int CGrenade_SpawnEntry(int e, gid_t16 gid)
{
	(void)gid;
	grenade_spawn(e);
	return 0;
}

static int CGrenade_BloodColor(int e)
{
	(void)e;
	return DONT_BLEED;
}

static const game_vt_t grenade_vt = {
	.BloodColor = CGrenade_BloodColor,
	.Killed = CGrenade_KilledVt,
};

const game_class_t gc_GRENADE = {
	.size = sizeof(CGrenade),
	.Spawn = CGrenade_SpawnEntry,
	.vt = &grenade_vt,
};

static int new_grenade(int owner, const float start[3])
{
	int e = CBaseEntity_Create(CLS_GRENADE, start, g_vecZero, owner);

	if (e >= 0)
		UTIL_SetOrigin(e, start);
	return e;
}

int CGrenade_ShootContact(int owner, const float start[3], const float vel[3])
{
	int e = new_grenade(owner, start);
	float av[3];

	if (e < 0)
		return -1;
	/* lower gravity since grenade is aerodynamic and engine doesn't know it */
	ent_phys(e)->gravity = 0.5f;
	ent_set_velocity(e, vel);
	UTIL_VecToAngles(vel, ent_cv(e)->angles);
	SetThink(e, FN_CGrenade_DangerSoundThink);
	ent_think_at(e, gtime());
	/* tumble in air */
	VecSet(av, RANDOM_FLOAT(-100, -500), 0, 0);
	ent_set_avelocity(e, av);
	/* explode on contact */
	SetTouch(e, FN_CGrenade_ExplodeTouch);
	G(e)->dmg = gSkillData.plrDmgM203Grenade;
	return e;
}

int CGrenade_ShootTimed(int owner, const float start[3], const float vel[3], float time)
{
	int e = new_grenade(owner, start);
	CGrenade *g;

	if (e < 0)
		return -1;
	g = G(e);
	ent_set_velocity(e, vel);
	UTIL_VecToAngles(vel, ent_cv(e)->angles);
	SetTouch(e, FN_CGrenade_BounceTouch);	/* bounce if touched */
	/* take one second off of the desired detonation time and set the think
	 * to PreDetonate. PreDetonate will insert a DANGER sound into the world
	 * sound list and delay detonation for one second so that monsters may
	 * get out of the way */
	g->dmgtime = gtime() + time;
	SetThink(e, FN_CGrenade_TumbleThink);
	ThinkIn(e, 0.1f);
	if (time < 0.1f)
	{
		ent_think_at(e, gtime());
		ent_set_velocity(e, g_vecZero);
	}
	ent_rnd(e)->sequence = (uint16_t)RANDOM_LONG(3, 6);
	ent_rnd(e)->framerate = 1.0f;
	/* tumble through the air */
	ent_phys(e)->gravity = 0.5f;
	ent_phys(e)->friction = 0.8f;
	ent_set_model(e, RESID(W_W_GRENADE));
	g->dmg = 100;
	return e;
}
