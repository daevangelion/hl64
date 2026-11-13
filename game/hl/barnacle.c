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
 * barnacle.c -- monster_barnacle (HLSDK dlls/barnacle.cpp CBarnacle).
 * The player's FBecomeProne / BarnacleVictimBitten / BarnacleVictimReleased
 * overrides (CBasePlayer, player.cpp) are Items' (game/hl/player.h); the
 * player's entvars the barnacle writes are its ent_player_t.
 */
#include <math.h>

#include "engine/phys/player.h"
#include "game/hl/monster.h"
#include "game/hl/player.h"

#define BARNACLE_BODY_HEIGHT		44
#define BARNACLE_PULL_SPEED		8
#define BARNACLE_AE_PUKEGIB		2
#define BARNACLE_CHECK_SPACING		8

typedef struct {
	CBASEENTITY_FIELDS
	CBASEDELAY_FIELDS
	CBASEANIMATING_FIELDS
	CBASETOGGLE_FIELDS
	CBASEMONSTER_FIELDS
	float		m_flAltitude;
	float		m_flKillVictimTime;
	int		m_cGibs;
	uint8_t		m_fTongueExtended;
	uint8_t		m_fLiftingPrey;
	int16_t		m_seqAttack1;	/* "attack1", looked up at spawn */
	float		m_flTongueAdj;
} CBarnacle;

#define BCL(e)	((CBarnacle *)ent_priv(e))

static const int16_t bclChew[] = { R_BCL_CHEW1, R_BCL_CHEW2, R_BCL_CHEW3 };
static const int16_t bclDie[] = { R_BCL_DIE1, R_BCL_DIE3 };

static int Bcl_Classify(int e) { (void)e; return CLASS_ALIEN_MONSTER; }

/* pev->deadflag of a client or monster */
static int victim_deadflag(int v)
{
	if (CBase(v))
		return CBase(v)->deadflag;
	return IsAlive(v) ? DEAD_NO : DEAD_DEAD;
}

static int victim_FBecomeProne(int v)
{
	if (IsPlayer(v))
		return Player_FBecomeProne(v);
	return Mon_FBecomeProne(v);
}

static void victim_Bitten(int v, int barnacle)
{
	if (IsPlayer(v))
		Player_BarnacleVictimBitten(v, barnacle);
	else if (mon_is_monster(v))
		Mon_BarnacleVictimBitten(v, barnacle);
}

static void victim_Released(int v)
{
	if (IsPlayer(v))
		Player_BarnacleVictimReleased(v);
	else if (mon_is_monster(v))
		Mon_BarnacleVictimReleased(v);
}

/* UTIL_SetOrigin on the victim (the player's origin is its ent_player_t) */
static void victim_set_origin(int v, const float org[3])
{
	if (IsPlayer(v))
		VecCopy(org, phy_player()->origin);
	UTIL_SetOrigin(v, org);
}

static void Bcl_HandleAnimEvent(int e, const MonsterEvent_t *ev)
{
	switch (ev->event)
	{
	case BARNACLE_AE_PUKEGIB:
		CGib_SpawnRandomGibs(e, 1, 1);
		break;
	default:
		CBaseMonster_HandleAnimEvent(e, ev);
		break;
	}
}

static int Bcl_TakeDamage(int e, int inflictor, int attacker, float flDamage, int bits)
{
	if (bits & DMG_CLUB)
		flDamage = BCL(e)->health;
	return CBaseMonster_TakeDamage(e, inflictor, attacker, flDamage, bits);
}

/* TongueTouchEnt: the first enemy client / monster along the tongue; the
 * tongue's length to the architecture below in *pflLength */
static int Bcl_TongueTouchEnt(int e, float *pflLength)
{
	const float	*o = ent_cv(e)->origin;
	col_trace_t	tr;
	float		end[3], mins[3], maxs[3], length;
	uint16_t	list[10];
	int		count, i;

	VecSet(end, o[0], o[1], o[2] - 2048);
	UTIL_TraceLine(o, end, ignore_monsters, e, &tr);
	length = fabsf(o[2] - tr.end[2]);
	if (pflLength)
		*pflLength = length;
	VecSet(mins, o[0] - BARNACLE_CHECK_SPACING, o[1] - BARNACLE_CHECK_SPACING, o[2] - length);
	VecSet(maxs, o[0] + BARNACLE_CHECK_SPACING, o[1] + BARNACLE_CHECK_SPACING, o[2]);
	count = UTIL_EntitiesInBox(list, 10, mins, maxs, FL_CLIENT | FL_MONSTER);
	for (i = 0; i < count; i++)
		if (list[i] != e && Mon_IRelationshipOf(e, list[i]) > R_NO &&
			victim_deadflag(list[i]) == DEAD_NO)
			return list[i];
	return -1;
}

void CBarnacle_BarnacleThink(int e)
{
	CBarnacle	*b = BCL(e);
	const float	*o = ent_cv(e)->origin;
	float		flLength, org[3], eye[3], yaw;
	int		touch, enemy;

	ThinkIn(e, 0.1f);
	enemy = EHGet(b->m_hEnemy);
	if (b->m_hEnemy != EH_NONE)
	{
		/* barnacle has prey */
		if (enemy < 0 || !IsAlive(enemy))
		{
			b->m_fLiftingPrey = 0;
			b->m_hEnemy = EH_NONE;
			return;
		}
		if (b->m_fLiftingPrey)
		{
			if (victim_deadflag(enemy) != DEAD_NO)
			{
				b->m_hEnemy = EH_NONE;
				b->m_fLiftingPrey = 0;
				return;
			}
			/* still pulling prey */
			VecCopy(ent_cv(enemy)->origin, org);
			org[0] = o[0];
			org[1] = o[1];
			yaw = ent_cv(enemy)->angles[1];
			org[0] -= 6 * (float)cos(yaw * M_PI / 180.0);
			org[1] -= 6 * (float)sin(yaw * M_PI / 180.0);
			b->m_flAltitude -= BARNACLE_PULL_SPEED;
			org[2] += BARNACLE_PULL_SPEED;
			EyePosition(enemy, eye);
			if (fabsf(o[2] - (org[2] + (eye[2] - ent_cv(enemy)->origin[2]) - 8)) <
				BARNACLE_BODY_HEIGHT)
			{
				/* prey has just been lifted into position */
				b->m_fLiftingPrey = 0;
				EMIT_SOUND(e, CHAN_WEAPON, RESID(BCL_BITE3), 1, ATTN_NORM);
				b->m_flKillVictimTime = gtime() + 10;
				if (mon_is_monster(enemy))
				{
					victim_Bitten(enemy, e);
					Mon_SetActivity(e, ACT_EAT);
				}
			}
			victim_set_origin(enemy, org);
		}
		else
		{
			/* prey is lifted fully into feeding position and dangling */
			int victim = mon_is_monster(enemy);

			if (b->m_flKillVictimTime != -1 && gtime() > b->m_flKillVictimTime)
			{
				if (victim)
				{
					TakeDamage(enemy, e, e, CBase(enemy) ? CBase(enemy)->health : 0,
						DMG_SLASH | DMG_ALWAYSGIB);
					b->m_cGibs = 3;
				}
				return;
			}
			if (victim && RANDOM_LONG(0, 49) == 0)
			{
				EMIT_SOUND(e, CHAN_WEAPON, RANDOM_SOUND_ARRAY(bclChew), 1, ATTN_NORM);
				victim_Bitten(enemy, e);
			}
		}
	}
	else
	{
		/* no prey: idle and check whether anything touches the tongue */
		if (ent_client_in_pvs(e) < 0)
			ThinkIn(e, RANDOM_FLOAT(1, 1.5f));
		if (b->anim.m_fSequenceFinished)
		{
			Mon_SetActivity(e, ACT_IDLE);
			b->m_flTongueAdj = -100;
		}
		if (b->m_cGibs && RANDOM_LONG(0, 99) == 1)
		{
			CGib_SpawnRandomGibs(e, 1, 1);
			b->m_cGibs--;
			EMIT_SOUND(e, CHAN_WEAPON, RANDOM_SOUND_ARRAY(bclChew), 1, ATTN_NORM);
		}
		touch = Bcl_TongueTouchEnt(e, &flLength);
		if (touch >= 0 && b->m_fTongueExtended)
		{
			/* tongue is fully extended and touching someone */
			if (victim_FBecomeProne(touch))
			{
				EMIT_SOUND(e, CHAN_WEAPON, RESID(BCL_ALERT2), 1, ATTN_NORM);
				if (b->m_seqAttack1 >= 0)
					Mon_SetSequenceIndex(e, b->m_seqAttack1);
				b->m_flTongueAdj = -20;
				b->m_hEnemy = EHANDLE(touch);
				VecCopy(ent_cv(touch)->origin, org);
				org[0] = o[0];
				org[1] = o[1];
				if (IsPlayer(touch))
				{
					ent_player_t *pl = phy_player();

					pl->movetype = PL_MOVE_FLY;
					VecSet(pl->velocity, 0, 0, 0);
					VecSet(pl->basevelocity, 0, 0, 0);
				}
				else
				{
					ent_set_movetype(touch, MOVETYPE_FLY);
					ent_set_velocity(touch, g_vecZero);
					ent_set_basevelocity(touch, g_vecZero);
				}
				victim_set_origin(touch, org);
				b->m_fLiftingPrey = 1;
				b->m_flKillVictimTime = -1;
				EyePosition(touch, eye);
				b->m_flAltitude = o[2] - eye[2];
			}
		}
		else
		{
			/* a new tongue length clear of anything else that moves under it */
			if (b->m_flAltitude < flLength)
			{
				b->m_flAltitude += BARNACLE_PULL_SPEED;
				b->m_fTongueExtended = 0;
			}
			else
			{
				b->m_flAltitude = flLength;
				b->m_fTongueExtended = 1;
			}
		}
	}
	Mon_SetBoneController(e, 0, -(b->m_flAltitude + b->m_flTongueAdj));
	Mon_StudioFrameAdvance(e, 0.1f);
}

static void Bcl_Killed(int e, int attacker, int iGib)
{
	CBarnacle	*b = BCL(e);
	int		enemy = EHGet(b->m_hEnemy);

	(void)attacker; (void)iGib;
	ent_set_solid(e, SOLID_NOT);
	b->takedamage = DAMAGE_NO;
	if (enemy >= 0)
		victim_Released(enemy);
	EMIT_SOUND(e, CHAN_WEAPON, RANDOM_SOUND_ARRAY(bclDie), 1, ATTN_NORM);
	Mon_SetActivity(e, ACT_DIESIMPLE);
	Mon_SetBoneController(e, 0, 0);
	Mon_StudioFrameAdvance(e, 0.1f);
	ThinkIn(e, 0.1f);
	SetThink(e, FN_CBarnacle_WaitTillDead);
}

void CBarnacle_WaitTillDead(int e)
{
	float flInterval;

	ThinkIn(e, 0.1f);
	flInterval = Mon_StudioFrameAdvance(e, 0.1f);
	Mon_DispatchAnimEvents(e, flInterval);
	if (BCL(e)->anim.m_fSequenceFinished)
	{
		/* StopAnimation */
		ent_rnd(e)->framerate = 0;
		SetThink(e, FN_NONE);
	}
}

static int CBarnacle_Spawn(int e, gid_t16 gid)
{
	CBaseMonster	*m = Mon_SpawnKeys(e, gid);
	CBarnacle	*b = (CBarnacle *)m;
	static const float mn[3] = { -16, -16, -32 }, mx[3] = { 16, 16, 0 };
	static const int16_t pre[] = {
		R_BCL_ALERT2, R_BCL_BITE3, R_BCL_CHEW1, R_BCL_CHEW2, R_BCL_CHEW3, R_BCL_DIE1,
		R_BCL_DIE3,
	};

	PRECACHE(RESID(MON_BARNACLE_MDL));
	PRECACHE_SOUND_ARRAY(pre);
	ent_set_model(e, RESID(MON_BARNACLE_MDL));
	ent_set_size(e, mn, mx);
	ent_set_solid(e, SOLID_SLIDEBOX);
	ent_set_movetype(e, MOVETYPE_NONE);
	m->takedamage = DAMAGE_AIM;
	m->m_bloodColor = BLOOD_COLOR_RED;
	ent_rnd(e)->effects = EF_INVLIGHT;
	m->health = 25;
	m->m_flFieldOfView = 0.5f;
	m->m_MonsterState = MONSTERSTATE_NONE;
	b->m_flKillVictimTime = 0;
	b->m_cGibs = 0;
	b->m_fLiftingPrey = 0;
	b->m_flTongueAdj = -100;
	b->m_seqAttack1 = (int16_t)Mon_LookupSequence(e, "attack1");
	Mon_InitBoneControllers(e);
	Mon_SetActivity(e, ACT_IDLE);
	SetThink(e, FN_CBarnacle_BarnacleThink);
	ThinkIn(e, 0.5f);
	UTIL_SetOrigin(e, ent_cv(e)->origin);
	return 0;
}

static const game_vt_t bcl_vt = {
	MON_GAME_VT_BASE,
	.TraceAttack = CBaseMonster_TraceAttack,
	.TakeDamage = Bcl_TakeDamage,
	.Killed = Bcl_Killed,
	.IRelationship = CBaseMonster_IRelationship,
	.Classify = Bcl_Classify,
};

static const mon_vt_t bcl_mvt = {
	.HandleAnimEvent = Bcl_HandleAnimEvent,
};

/* HLSDK CBarnacle::m_SaveData */
static const sv_field_t f_CBarnacle[] = {
	SVF(CBarnacle, m_flAltitude, SV_F32, 0),
	SVF(CBarnacle, m_flKillVictimTime, SV_TIME, 0),
	SVF(CBarnacle, m_cGibs, SV_I32, 0),
	SVF(CBarnacle, m_fTongueExtended, SV_U8, 0),
	SVF(CBarnacle, m_fLiftingPrey, SV_U8, 0),
	SVF(CBarnacle, m_seqAttack1, SV_I16, SVK),
	SVF(CBarnacle, m_flTongueAdj, SV_F32, 0),
};
static const sv_table_t sv_CBarnacle = SV_TABLE(&sv_CBaseMonster, f_CBarnacle);

const game_class_t gc_MONSTER_BARNACLE = {
	.size = sizeof(CBarnacle),
	.Spawn = CBarnacle_Spawn,
	.vt = &bcl_vt,
	.ext = &bcl_mvt,
	.save = &sv_CBarnacle,
};
