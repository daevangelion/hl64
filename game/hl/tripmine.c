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
 * off; a mount that moves while it powers up turns it back into a
 * weapon_tripmine. The player's tripmine weapon is weapon_tripmine.c.
 */
#include "game/hl/grenade.h"
#include "game/hl/player.h"
#include "game/hl/savegame.h"

#define TRIPMINE_WORLD		7

typedef struct {
	CGrenade	g;
	float		m_flPowerUp;
	float		m_vecDir[3];
	float		m_vecEnd[3];
	float		m_flBeamLength;
	ent_h		m_hOwner;
	int		m_pBeam;		/* fx beam key, -1 none */
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

/* the laser at the measured length: CBeam::BeamCreate(laserbeam, 10)
 * PointEntInit(end, self), colour 0 214 198, scroll 255, brightness 64 */
static int beam_issue(int e)
{
	CTripmineGrenade *t = TM(e);
	float end[3];

	VecMA(ent_cv(e)->origin, 2048 * t->m_flBeamLength, t->m_vecDir, end);
	return fx_beam(FX_BEAM_KEY(e, 0), -1, end, e, NULL, RESID(SPR_LASERBEAM), 0, 0, 10, 0, 0,
		214, 198, 64, 255, 0, 0);
}

static void MakeBeam(int e)
{
	CTripmineGrenade *t = TM(e);
	col_trace_t tr;

	UTIL_TraceLine(ent_cv(e)->origin, t->m_vecEnd, dont_ignore_monsters, e, &tr);
	t->m_flBeamLength = tr.frac;
	SetThink(e, FN_CTripmineGrenade_BeamBreakThink);
	ThinkIn(e, 0.1f);
	t->m_pBeam = beam_issue(e);
	log_printf("tripmine: %d powered up at %.3f, beam %d length %.1f\n", e, (double)gtime(),
		t->m_pBeam, (double)(2048 * t->m_flBeamLength));
}

/* After a saved game's load (frame layer: the mine's): the laser made
 * again at the saved length, without MakeBeam's trace or think. A mine
 * whose beam the saving game had (m_pBeam >= 0) keeps one: BeamBreakThink
 * remakes a beam (re-measuring, re-owning) only for m_pBeam < 0, a
 * simulation decision. The key is the mine's (also with a full beam
 * pool), so m_pBeam is the saved value. */
void CTripmineGrenade_LoadDone(int e)
{
	CTripmineGrenade *t = TM(e);

	if (t->m_pBeam < 0)
		return;
	t->m_pBeam = beam_issue(e);
}

/* m_hOwner as CBaseEntity::Instance does: the world (slot 0, no live
 * entity in hl64) is an owner that never moves */
static int owner_of(ent_h h)
{
	return h == EHANDLE(ENT_WORLD) ? ENT_WORLD : EHGet(h);
}

static int owner_moved(CTripmineGrenade *t)
{
	int o = owner_of(t->m_hOwner);
	static const float zero[3] = { 0, 0, 0 };

	if (o < 0)
		return 1;
	return !VecEq(t->m_posOwner, o == ENT_WORLD ? zero : ent_cv(o)->origin) ||
		!VecEq(t->m_angleOwner, o == ENT_WORLD ? zero : ent_cv(o)->angles);
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
			if (o == ENT_WORLD)
			{
				VecSet(t->m_posOwner, 0, 0, 0);
				VecSet(t->m_angleOwner, 0, 0, 0);
			}
			else
			{
				VecCopy(ent_cv(o)->origin, t->m_posOwner);
				VecCopy(ent_cv(o)->angles, t->m_angleOwner);
			}
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
		/* disable */
		float org[3];
		int mine;

		STOP_SOUND(e, CHAN_VOICE, RESID(W_MINE_DEPLOY));
		STOP_SOUND(e, CHAN_BODY, RESID(W_MINE_CHARGE));
		VecMA(c->origin, 24, t->m_vecDir, org);
		mine = CBaseEntity_Create(CLS_WEAPON_TRIPMINE, org, c->angles, -1);
		if (mine >= 0)
			CBase(mine)->spawnflags |= SF_NORESPAWN;
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
		log_printf("tripmine: %d beam broken at %.3f (hit %d)\n", e, (double)gtime(),
			tr.ent);
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

/* HLSDK CTripmineGrenade::m_SaveData. The beam is an effect (HLSDK saves
 * the beam entity): killed with the release; after a restore m_pBeam is
 * -1 and BeamBreakThink's "respawn detect" makes it again. A mine powering
 * up or about to blow is not quiescent. */
static int CTripmineGrenade_quiet(int e)
{
	fn_t t = ent_tm(e)->think;

	return t != FN_CTripmineGrenade_PowerupThink && t != FN_CTripmineGrenade_DelayDeathThink;
}

static const sv_field_t f_CTripmineGrenade[] = {
	SVF(CTripmineGrenade, m_flPowerUp, SV_TIME, 0),
	SVF(CTripmineGrenade, m_vecDir, SV_VEC, 0),
	SVF(CTripmineGrenade, m_vecEnd, SV_POS, 0),
	SVF(CTripmineGrenade, m_flBeamLength, SV_F32, 0),
	SVF(CTripmineGrenade, m_hOwner, SV_EH, 0),
	SVF(CTripmineGrenade, m_posOwner, SV_POS, 0),
	SVF(CTripmineGrenade, m_angleOwner, SV_VEC, 0),
	SVF(CTripmineGrenade, m_pRealOwner, SV_IX32, 0),
};
static const sv_table_t sv_CTripmineGrenade = SV_TABLE(&sv_CGrenade, f_CTripmineGrenade,
	.quiet = CTripmineGrenade_quiet, .released = KillBeam);

const game_class_t gc_MONSTER_TRIPMINE = {
	.size = sizeof(CTripmineGrenade),
	.Spawn = CTripmineGrenade_Spawn,
	.vt = &tripmine_vt,
	.save = &sv_CTripmineGrenade,
};
