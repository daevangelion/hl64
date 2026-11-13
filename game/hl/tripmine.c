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
 * tripmine.c -- monster_tripmine (CTripmineGrenade, HLSDK dlls/tripmine.cpp):
 * powers up against the surface behind it, then a laser beam
 * (fx_beam, sprites/laserbeam.spr) whose break or a moved mount sets it
 * off. weapon_tripmine (the player's tripmine weapon) is not in
 * classes.def: Uplink / Hazard do not hand it out, so a moved mount just
 * removes the mine instead of dropping a weapon_tripmine.
 */
#include "game/hl/grenade.h"
#include "game/hl/player.h"

#define TRIPMINE_WORLD		7

typedef struct {
	CGrenade	g;
	float		m_flPowerUp;
	float		m_vecDir[3];
	float		m_vecEnd[3];
	float		m_flBeamLength;
	ent_h		m_hOwner;
	int		m_pBeam;		/* fx beam id, -1 none */
	float		m_posOwner[3];
	float		m_angleOwner[3];
	int		m_pRealOwner;		/* entity slot, -1 none */
} CTripmineGrenade;

#define TM(e)	PRIV(CTripmineGrenade, e)

static void KillBeam(int e)
{
	CTripmineGrenade *t = TM(e);

	if (t->m_pBeam >= 0)
	{
		fx_beam_kill(t->m_pBeam);
		t->m_pBeam = -1;
	}
}

static void MakeBeam(int e)
{
	CTripmineGrenade *t = TM(e);
	col_trace_t tr;
	float end[3];

	UTIL_TraceLine(ent_cv(e)->origin, t->m_vecEnd, dont_ignore_monsters, e, &tr);
	t->m_flBeamLength = tr.frac;
	SetThink(e, FN_CTripmineGrenade_BeamBreakThink);
	ThinkIn(e, 0.1f);
	VecMA(ent_cv(e)->origin, 2048 * t->m_flBeamLength, t->m_vecDir, end);
	/* CBeam::BeamCreate(laserbeam, 10) PointEntInit(end, self), colour
	 * 0 214 198, scroll 255, brightness 64 */
	t->m_pBeam = fx_beam(-1, end, e, NULL, RESID(SPR_LASERBEAM), 10, 0, 0, 214, 198, 64, 255, 0);
}

static int owner_moved(CTripmineGrenade *t)
{
	int o = EHGet(t->m_hOwner);

	return o < 0 || !VecEq(t->m_posOwner, ent_cv(o)->origin) ||
		!VecEq(t->m_angleOwner, ent_cv(o)->angles);
}

void CTripmineGrenade_PowerupThink(int e)
{
	CTripmineGrenade *t = TM(e);
	col_ent_t *c = ent_cv(e);
	col_trace_t tr;

	if (t->m_hOwner == EH_NONE)
	{
		/* find an owner */
		uint16_t oldowner = c->owner;
		float a[3], b[3];

		c->owner = COL_NONE;
		VecMA(c->origin, 8, t->m_vecDir, a);
		VecMA(c->origin, -32, t->m_vecDir, b);
		UTIL_TraceLine(a, b, dont_ignore_monsters, e, &tr);
		if (tr.startsolid || (oldowner != COL_NONE && tr.ent == oldowner))
		{
			c->owner = oldowner;
			t->m_flPowerUp += 0.1f;
			ThinkIn(e, 0.1f);
			return;
		}
		if (tr.frac < 1.0f)
		{
			int o = tr.ent >= 0 ? tr.ent : 0;

			c->owner = (uint16_t)o;
			t->m_hOwner = EHANDLE(o);
			VecCopy(ent_cv(o)->origin, t->m_posOwner);
			VecCopy(ent_cv(o)->angles, t->m_angleOwner);
		}
		else
		{
			STOP_SOUND(e, CHAN_VOICE, RESID(W_MINE_DEPLOY));
			STOP_SOUND(e, CHAN_BODY, RESID(W_MINE_CHARGE));
			SetThink(e, FN_SUB_Remove);
			ThinkIn(e, 0.1f);
			log_printf("game: WARNING:Tripmine at %.0f, %.0f, %.0f removed\n",
				(double)c->origin[0], (double)c->origin[1], (double)c->origin[2]);
			KillBeam(e);
			return;
		}
	}
	else if (owner_moved(t))
	{
		/* the mount moved: HLSDK drops a weapon_tripmine (not in the game) */
		STOP_SOUND(e, CHAN_VOICE, RESID(W_MINE_DEPLOY));
		STOP_SOUND(e, CHAN_BODY, RESID(W_MINE_CHARGE));
		SetThink(e, FN_SUB_Remove);
		KillBeam(e);
		ThinkIn(e, 0.1f);
		return;
	}
	if (gtime() > t->m_flPowerUp)
	{
		/* make solid */
		ent_set_solid(e, SOLID_BBOX);
		UTIL_SetOrigin(e, c->origin);
		MakeBeam(e);
		/* play enabled sound */
		EMIT_SOUND_DYN(e, CHAN_VOICE, RESID(W_MINE_ACTIVATE), 0.5f, ATTN_NORM, 1, 75);
	}
	ThinkIn(e, 0.1f);
}

static void CTripmineGrenade_Killed(int e, int attacker, int gib)
{
	CTripmineGrenade *t = TM(e);

	(void)gib;
	t->g.takedamage = DAMAGE_NO;
	if (attacker >= 0 && (IsPlayer(attacker) || (ent_cv(attacker)->flags & FL_CLIENT)))
		/* some client has destroyed this mine, he'll get credit for any kills */
		ent_cv(e)->owner = (uint16_t)attacker;
	SetThink(e, FN_CTripmineGrenade_DelayDeathThink);
	ThinkIn(e, RANDOM_FLOAT(0.1f, 0.3f));
	EMIT_SOUND(e, CHAN_BODY, RESID(SND_NULL), 0.5f, ATTN_NORM);	/* shut off chargeup */
}

void CTripmineGrenade_BeamBreakThink(int e)
{
	CTripmineGrenade *t = TM(e);
	col_trace_t tr;
	int bBlowup = 0;

	/* HACKHACK set to a simple box (FTRACE_SIMPLEBOX): the line hits
	 * bounding boxes; hl64 line traces refine to hitboxes, which only
	 * narrows what breaks the beam */
	UTIL_TraceLine(ent_cv(e)->origin, t->m_vecEnd, dont_ignore_monsters, e, &tr);
	/* respawn detect */
	if (t->m_pBeam < 0)
	{
		MakeBeam(e);
		if (tr.ent >= 0)
			t->m_hOwner = EHANDLE(tr.ent);	/* reset owner too */
	}
	if (fabsf(t->m_flBeamLength - tr.frac) > 0.001f)
		bBlowup = 1;
	else if (owner_moved(t))
		bBlowup = 1;
	if (bBlowup)
	{
		/* a trace line doesn't hit pev->owner: the real owner gets credit */
		ent_cv(e)->owner = t->m_pRealOwner >= 0 ? (uint16_t)t->m_pRealOwner : COL_NONE;
		t->g.health = 0;
		CTripmineGrenade_Killed(e, t->m_pRealOwner, GIB_NORMAL);
		return;
	}
	ThinkIn(e, 0.1f);
}

void CTripmineGrenade_DelayDeathThink(int e)
{
	CTripmineGrenade *t = TM(e);
	col_trace_t tr;
	float a[3], b[3];

	KillBeam(e);
	VecMA(ent_cv(e)->origin, 8, t->m_vecDir, a);
	VecMA(ent_cv(e)->origin, -64, t->m_vecDir, b);
	UTIL_TraceLine(a, b, dont_ignore_monsters, e, &tr);
	CGrenade_Explode(e, &tr, DMG_BLAST);
}

static int CTripmineGrenade_TakeDamage(int e, int inflictor, int attacker, float flDamage,
	int bits)
{
	CTripmineGrenade *t = TM(e);

	(void)inflictor; (void)bits;
	if (gtime() < t->m_flPowerUp && flDamage < t->g.health)
	{
		/* disable */
		SetThink(e, FN_SUB_Remove);
		ThinkIn(e, 0.1f);
		KillBeam(e);
		return 0;
	}
	/* CGrenade (CBaseMonster) TakeDamage for a non-monster */
	if (!t->g.takedamage)
		return 0;
	t->g.health -= flDamage;
	if (t->g.health <= 0)
	{
		CTripmineGrenade_Killed(e, attacker, GIB_NORMAL);
		return 0;
	}
	return 1;
}

static int CTripmineGrenade_BloodColor(int e)
{
	(void)e;
	return DONT_BLEED;
}

static int CTripmineGrenade_Spawn(int e, gid_t16 gid)
{
	CTripmineGrenade *t = TM(e);
	col_ent_t *c = ent_cv(e);
	static const float mins[3] = { -8, -8, -8 }, maxs[3] = { 8, 8, 8 };

	(void)gid;
	t->m_pBeam = -1;
	t->m_hOwner = EH_NONE;
	t->m_pRealOwner = -1;
	ent_set_movetype(e, MOVETYPE_FLY);
	ent_set_solid(e, SOLID_NOT);
	ent_set_model(e, RESID(W_V_TRIPMINE));
	ent_rnd(e)->frame = 0;
	ent_rnd(e)->body = 3;
	ent_rnd(e)->sequence = TRIPMINE_WORLD;
	gs_reset_sequence_info(e, &t->g.anim);
	ent_rnd(e)->framerate = 0;
	UTIL_SetSize(e, mins, maxs);
	UTIL_SetOrigin(e, c->origin);
	/* power up quickly (spawnflags 1) or slowly */
	t->m_flPowerUp = gtime() + ((t->g.spawnflags & 1) ? 1.0f : 2.5f);
	SetThink(e, FN_CTripmineGrenade_PowerupThink);
	ThinkIn(e, 0.2f);
	t->g.takedamage = DAMAGE_YES;
	t->g.dmg = gSkillData.plrDmgTripmine;
	t->g.health = 1;	/* don't let die normally */
	if (c->owner != COL_NONE)
	{
		/* play deploy sound, chargeup */
		EMIT_SOUND(e, CHAN_VOICE, RESID(W_MINE_DEPLOY), 1.0f, ATTN_NORM);
		EMIT_SOUND(e, CHAN_BODY, RESID(W_MINE_CHARGE), 0.2f, ATTN_NORM);
		t->m_pRealOwner = c->owner;
	}
	UTIL_MakeAimVectors(c->angles);
	VecCopy(gpGlobals.v_forward, t->m_vecDir);
	VecMA(c->origin, 2048, t->m_vecDir, t->m_vecEnd);
	return 0;
}

static const game_vt_t tripmine_vt = {
	.BloodColor = CTripmineGrenade_BloodColor,
	.TakeDamage = CTripmineGrenade_TakeDamage,
	.Killed = CTripmineGrenade_Killed,
};

const game_class_t gc_MONSTER_TRIPMINE = {
	.size = sizeof(CTripmineGrenade),
	.Spawn = CTripmineGrenade_Spawn,
	.vt = &tripmine_vt,
};
