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
 * roach.c -- monster_cockroach (HLSDK dlls/roach.cpp CRoach): its own
 * MonsterThink (no schedules), light-shy, food-seeking.
 */
#include <math.h>

#include "engine/phys/player.h"
#include "game/hl/monster.h"

#define ROACH_IDLE		0
#define ROACH_BORED		1
#define ROACH_SCARED_BY_ENT	2
#define ROACH_SCARED_BY_LIGHT	3
#define ROACH_SMELL_FOOD	4
#define ROACH_EAT		5

typedef struct {
	CBASEENTITY_FIELDS
	CBASEDELAY_FIELDS
	CBASEANIMATING_FIELDS
	CBASETOGGLE_FIELDS
	CBASEMONSTER_FIELDS
	float		m_flLastLightLevel;
	float		m_flNextSmellTime;
	uint8_t		m_fLightHacked;
	uint8_t		roach_pad;
	int16_t		m_iMode;
} CRoach;

#define ROACH(e)	((CRoach *)ent_priv(e))

static int Roach_ISoundMask(int e) { (void)e; return bits_SOUND_CARCASS | bits_SOUND_MEAT; }
static int Roach_Classify(int e) { (void)e; return CLASS_INSECT; }
static void Roach_SetYawSpeed(int e) { ent_phys(e)->yaw_speed = 120; }

static int roach_pitch(void) { return 80 + RANDOM_LONG(0, 39); }

void CRoach_Touch(int e, int other)
{
	col_trace_t	tr;
	float		spot[3], end[3];

	/* the player's pev->velocity is its ent_player_t */
	if (!IsPlayer(other) || VecIsZero(phy_player()->velocity))
		return;
	VecCopy(ent_cv(e)->origin, spot);
	spot[2] += 8;
	VecCopy(spot, end);
	end[2] -= 24;
	UTIL_TraceLine(spot, end, ignore_monsters, e, &tr);
	/* not really blood: no UTIL_ShouldShowBlood */
	UTIL_DecalTrace(&tr, DECAL_YBLOOD1 + RANDOM_LONG(0, 5));
	TakeDamage(e, other, other, ROACH(e)->health, DMG_CRUSH);
}

static void Roach_Killed(int e, int attacker, int iGib)
{
	CRoach	*r = ROACH(e);
	int	owner;

	(void)attacker; (void)iGib;
	ent_set_solid(e, SOLID_NOT);
	if (RANDOM_LONG(0, 4) == 1)
		EMIT_SOUND_DYN(e, CHAN_VOICE, RESID(RCH_DIE), 0.8f, ATTN_NORM, 0, roach_pitch());
	else
		EMIT_SOUND_DYN(e, CHAN_BODY, RESID(RCH_SMASH), 0.7f, ATTN_NORM, 0, roach_pitch());
	CSoundEnt_InsertSound(bits_SOUND_WORLD, ent_cv(e)->origin, 128, 1);
	/* pOwner->DeathNotice: a monstermaker child (record) or an owner entity */
	if (r->m_makerGid != GID_NONE)
		Monstermaker_DeathNotice(r->m_makerGid, e);
	owner = ent_cv(e)->owner;
	if (owner != COL_NONE && ent_is_live(owner) && ent_vt(owner) && ent_vt(owner)->DeathNotice)
		ent_vt(owner)->DeathNotice(owner, e);
	UTIL_Remove(e);
}

/* PickNewDest: a spot to run to (the food, else a random spot >= 128 u away) */
static void Roach_PickNewDest(int e, int iCondition)
{
	CRoach		*r = ROACH(e);
	const float	*o = ent_cv(e)->origin;
	CSound		*snd;
	float		dir[3], dest[3], d[3], flDist;

	r->m_iMode = (int16_t)iCondition;
	if (r->m_iMode == ROACH_SMELL_FOOD)
	{
		snd = CSoundEnt_SoundPointerForIndex(r->m_iAudibleList);
		if (snd)
		{
			r->m_Route[0].vecLocation[0] = snd->m_vecOrigin[0] + (3 - RANDOM_LONG(0, 5));
			r->m_Route[0].vecLocation[1] = snd->m_vecOrigin[1] + (3 - RANDOM_LONG(0, 5));
			r->m_Route[0].vecLocation[2] = snd->m_vecOrigin[2];
			r->m_Route[0].iType = bits_MF_TO_LOCATION;
			r->m_movementGoal = Mon_RouteClassify(e, r->m_Route[0].iType);
			return;
		}
	}
	do
	{
		dir[0] = RANDOM_FLOAT(-1, 1);
		dir[1] = RANDOM_FLOAT(-1, 1);
		dir[2] = 0;
		flDist = 256 + RANDOM_LONG(0, 255);
		VecMA(o, flDist, dir, dest);
		VecSub(dest, o, d);
	} while (VecLen2D(d) < 128);
	r->m_Route[0].vecLocation[0] = dest[0];
	r->m_Route[0].vecLocation[1] = dest[1];
	r->m_Route[0].vecLocation[2] = o[2];
	r->m_Route[0].iType = bits_MF_TO_LOCATION;
	r->m_movementGoal = Mon_RouteClassify(e, r->m_Route[0].iType);
	if (RANDOM_LONG(0, 9) == 1)
		EMIT_SOUND_DYN(e, CHAN_BODY, RESID(RCH_WALK), 1, ATTN_NORM, 0, roach_pitch());
}

static void Roach_Move(int e, float flInterval)
{
	CRoach		*r = ROACH(e);
	const float	*loc = r->m_Route[r->m_iRouteIndex].vecLocation;
	float		d[3], flWaypointDist, ideal;

	VecSub(loc, ent_cv(e)->origin, d);
	flWaypointDist = VecLen2D(d);
	Mon_MakeIdealYaw(e, loc);
	Mon_ChangeYaw(e, (int)ent_phys(e)->yaw_speed);
	UTIL_MakeVectors(ent_cv(e)->angles);
	ideal = ent_phys(e)->ideal_yaw;
	/* randomly check for a blocked path */
	if (RANDOM_LONG(0, 7) == 1 && !phy_walk_move(e, ideal, 4, WALKMOVE_NORMAL))
		Roach_PickNewDest(e, r->m_iMode);
	phy_walk_move(e, ideal, r->anim.m_flGroundSpeed * flInterval, WALKMOVE_NORMAL);
	/* waypoint closer than the step: stop after it (overshooting is fine) */
	if (flWaypointDist <= r->anim.m_flGroundSpeed * flInterval)
	{
		Mon_SetActivity(e, ACT_IDLE);
		r->m_flLastLightLevel = (float)ent_illum(e);
		r->m_iMode = r->m_iMode == ROACH_SMELL_FOOD ? ROACH_EAT : ROACH_IDLE;
	}
	if (RANDOM_LONG(0, 149) == 1 && r->m_iMode != ROACH_SCARED_BY_LIGHT &&
		r->m_iMode != ROACH_SMELL_FOOD)
		Roach_PickNewDest(e, 0);
}

/* Look: a 360 degree radius look, fear only */
static void Roach_Look(int e, int iDistance)
{
	CRoach	*r = ROACH(e);
	int	s = -1, iSighted = 0;

	Mon_ClearConditions((CBaseMonster *)r, bits_COND_SEE_HATE | bits_COND_SEE_DISLIKE |
		bits_COND_SEE_ENEMY | bits_COND_SEE_FEAR);
	if (ent_client_in_pvs(e) < 0)
		return;
	while ((s = UTIL_FindEntityInSphere(s, ent_cv(e)->origin, (float)iDistance)) >= 0)
	{
		if (!IsPlayer(s) && !(ent_cv(s)->flags & FL_MONSTER))
			continue;
		if ((ent_cv(s)->flags & FL_NOTARGET) || !IsAlive(s))
			continue;
		switch (Mon_IRelationshipOf(e, s))
		{
		case R_FR:
			iSighted |= bits_COND_SEE_FEAR;
			break;
		case R_NO:
			break;
		default:
			log_printf("monster_cockroach can't asses entity %d\n", s);
			break;
		}
	}
	Mon_SetConditions((CBaseMonster *)r, iSighted);
}

void CRoach_MonsterThink(int e)
{
	CRoach	*r = ROACH(e);
	float	flInterval;
	CSound	*snd;

	if (ent_client_in_pvs(e) < 0)
		ThinkIn(e, RANDOM_FLOAT(1, 1.5f));
	else
		ThinkIn(e, 0.1f);
	flInterval = Mon_StudioFrameAdvance(e, 0);
	if (!r->m_fLightHacked)
	{
		/* let the world finish spawning, then sample the light level */
		ThinkIn(e, 1);
		r->m_fLightHacked = 1;
		return;
	}
	else if (r->m_flLastLightLevel < 0)
		r->m_flLastLightLevel = (float)ent_illum(e);
	switch (r->m_iMode)
	{
	case ROACH_IDLE:
	case ROACH_EAT:
		if (RANDOM_LONG(0, 3) == 1)
		{
			Mon_Look(e, 150);
			if (Mon_HasConditions((CBaseMonster *)r, bits_COND_SEE_FEAR))
			{
				Mon_Eat(e, 30 + RANDOM_LONG(0, 14));
				Roach_PickNewDest(e, ROACH_SCARED_BY_ENT);
				Mon_SetActivity(e, ACT_WALK);
			}
			else if (RANDOM_LONG(0, 149) == 1)
			{
				Roach_PickNewDest(e, ROACH_BORED);
				Mon_SetActivity(e, ACT_WALK);
				if (r->m_iMode == ROACH_EAT)
					Mon_Eat(e, 30 + RANDOM_LONG(0, 14));
			}
		}
		if (r->m_iMode == ROACH_IDLE)
		{
			if (Mon_FShouldEat(e))
				Mon_Listen(e);
			if ((float)ent_illum(e) > r->m_flLastLightLevel)
			{
				/* someone turned on lights */
				Roach_PickNewDest(e, ROACH_SCARED_BY_LIGHT);
				Mon_SetActivity(e, ACT_WALK);
			}
			else if (Mon_HasConditions((CBaseMonster *)r, bits_COND_SMELL_FOOD))
			{
				snd = CSoundEnt_SoundPointerForIndex(r->m_iAudibleList);
				if (snd && fabsf(snd->m_vecOrigin[2] - ent_cv(e)->origin[2]) <= 3)
				{
					Roach_PickNewDest(e, ROACH_SMELL_FOOD);
					Mon_SetActivity(e, ACT_WALK);
				}
			}
		}
		break;
	case ROACH_SCARED_BY_LIGHT:
		/* stop over a spot at least as dark as where it started */
		if ((float)ent_illum(e) <= r->m_flLastLightLevel)
		{
			Mon_SetActivity(e, ACT_IDLE);
			r->m_flLastLightLevel = (float)ent_illum(e);
		}
		break;
	}
	if (r->anim.m_flGroundSpeed != 0)
		Roach_Move(e, flInterval);
}

static int CRoach_Spawn(int e, gid_t16 gid)
{
	CBaseMonster	*m = Mon_SpawnKeys(e, gid);
	CRoach		*r = (CRoach *)m;
	static const float mn[3] = { -1, -1, 0 }, mx[3] = { 1, 1, 2 };

	PRECACHE(RESID(MON_ROACH_MDL));
	PRECACHE(RESID(RCH_DIE));
	PRECACHE(RESID(RCH_WALK));
	PRECACHE(RESID(RCH_SMASH));
	ent_set_model(e, RESID(MON_ROACH_MDL));
	ent_set_size(e, mn, mx);
	ent_set_solid(e, SOLID_SLIDEBOX);
	ent_set_movetype(e, MOVETYPE_STEP);
	m->m_bloodColor = BLOOD_COLOR_YELLOW;
	ent_rnd(e)->effects = 0;
	m->health = 1;
	m->m_flFieldOfView = 0.5f;
	m->m_MonsterState = MONSTERSTATE_NONE;
	Mon_MonsterInit(e);
	Mon_SetActivity(e, ACT_IDLE);
	VecSet(m->view_ofs, 0, 0, 1);
	ent_phys(e)->view_ofs_z = 1;
	m->takedamage = DAMAGE_YES;
	r->m_fLightHacked = 0;
	r->m_flLastLightLevel = -1;
	r->m_iMode = ROACH_IDLE;
	r->m_flNextSmellTime = gtime();
	/* the CRoach MonsterThink / Touch overrides */
	SetTouch(e, FN_CRoach_Touch);
	return 0;
}

/* StartMonster (MonsterInit's think) installs MonsterThink: the roach's */
static void Roach_StartMonster(int e)
{
	CBaseMonster_StartMonster(e);
	SetThink(e, FN_CRoach_MonsterThink);
}

static const game_vt_t roach_vt = {
	MON_GAME_VT_BASE,
	.TraceAttack = CBaseMonster_TraceAttack,
	.TakeDamage = CBaseMonster_TakeDamage,
	.Killed = Roach_Killed,
	.IRelationship = CBaseMonster_IRelationship,
	.Classify = Roach_Classify,
};

static const mon_vt_t roach_mvt = {
	.Look = Roach_Look,
	.SetYawSpeed = Roach_SetYawSpeed,
	.ISoundMask = Roach_ISoundMask,
	.StartMonster = Roach_StartMonster,
};

const game_class_t gc_MONSTER_COCKROACH = {
	.size = sizeof(CRoach),
	.Spawn = CRoach_Spawn,
	.vt = &roach_vt,
	.ext = &roach_mvt,
};
