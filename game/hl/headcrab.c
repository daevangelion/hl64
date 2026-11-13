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
 * headcrab.c -- monster_headcrab (HLSDK dlls/headcrab.cpp CHeadCrab) and
 * monster_zombie (dlls/zombie.cpp CZombie). monster_babycrab is not a class
 * of the Uplink / Hazard maps.
 */
#include <math.h>

#include "game/hl/monster.h"

#define HC_AE_JUMPATTACK	2

static const float sk_headcrabHealth[3] = { 10, 10, 20 };
static const float sk_headcrabDmgBite[3] = { 5, 10, 10 };

#define T(t, d)	{ t, (float)(d) }

static const Task_t tlHCRangeAttack1[] = {
	T(TASK_STOP_MOVING, 0), T(TASK_FACE_IDEAL, 0), T(TASK_RANGE_ATTACK1, 0),
	T(TASK_SET_ACTIVITY, ACT_IDLE), T(TASK_FACE_IDEAL, 0), T(TASK_WAIT_RANDOM, 0.5),
};
static const Schedule_t slHCRangeAttack1[] = {
	SCHED(tlHCRangeAttack1, bits_COND_ENEMY_OCCLUDED | bits_COND_NO_AMMO_LOADED, 0, "HCRangeAttack1")
};

static const int16_t hcIdle[] = { R_HC_IDLE1, R_HC_IDLE2, R_HC_IDLE3 };
static const int16_t hcAlert[] = { R_HC_ALERT1 };
static const int16_t hcPain[] = { R_HC_PAIN1, R_HC_PAIN2, R_HC_PAIN3 };
static const int16_t hcAttack[] = { R_HC_ATTACK1, R_HC_ATTACK2, R_HC_ATTACK3 };
static const int16_t hcDeath[] = { R_HC_DIE1, R_HC_DIE2 };
static const int16_t hcBite[] = { R_HC_HEADBITE };

static int HC_Classify(int e) { (void)e; return CLASS_ALIEN_PREY; }

static void HC_BodyTarget(int e, const float src[3], float out[3])
{
	(void)src;
	VecCopy(ent_cv(e)->origin, out);	/* Center(): origin + 6 */
	out[2] += 6;
}

static void HC_SetYawSpeed(int e)
{
	int ys;

	switch (MON(e)->m_Activity)
	{
	case ACT_RUN:
	case ACT_WALK: ys = 20; break;
	case ACT_TURN_LEFT:
	case ACT_TURN_RIGHT: ys = 60; break;
	default: ys = 30; break;
	}
	ent_phys(e)->yaw_speed = (float)ys;
}

static void HC_HandleAnimEvent(int e, const MonsterEvent_t *ev)
{
	CBaseMonster	*m = MON(e);
	float		org[3], dir[3], gravity, height, speed, time, dist;
	int		en, s;

	if (ev->event != HC_AE_JUMPATTACK)
	{
		CBaseMonster_HandleAnimEvent(e, ev);
		return;
	}
	EFlagsClear(e, FL_ONGROUND);
	VecCopy(ent_cv(e)->origin, org);
	org[2] += 1;
	ent_set_origin(e, org);
	UTIL_MakeVectors(ent_cv(e)->angles);
	en = EHGet(m->m_hEnemy);
	if (en >= 0)
	{
		const float *vo = ent_priv(en) ? CBase(en)->view_ofs : g_vecZero;

		gravity = settings.gravity;
		if (gravity <= 1)
			gravity = 1;
		height = ent_cv(en)->origin[2] + vo[2] - ent_cv(e)->origin[2];
		if (height < 16)
			height = 16;
		speed = sqrtf(2 * gravity * height);
		time = speed / gravity;
		VecAdd(ent_cv(en)->origin, vo, dir);
		VecSub(dir, ent_cv(e)->origin, dir);
		VecScale(dir, 1.0f / time, dir);
		dir[2] = speed;
		dist = VecLen(dir);
		if (dist > 650)
			VecScale(dir, 650.0f / dist, dir);
	}
	else
	{
		VecSet(dir, gpGlobals.v_forward[0], gpGlobals.v_forward[1], gpGlobals.v_up[2]);
		VecScale(dir, 350, dir);
	}
	s = RANDOM_LONG(0, 2);
	if (s != 0)
		EMIT_SOUND_DYN(e, CHAN_VOICE, res_def(hcAttack[s]), 1.0f, ATTN_IDLE, 0, 100);
	ent_set_velocity(e, dir);
	m->m_flNextAttack = gtime() + 2;
}

static void HC_RunTask(int e, const Task_t *t)
{
	CBaseMonster *m = MON(e);

	switch (t->iTask)
	{
	case TASK_RANGE_ATTACK1:
	case TASK_RANGE_ATTACK2:
		if (m->anim.m_fSequenceFinished)
		{
			Mon_TaskComplete(m);
			SetTouch(e, FN_NONE);
			m->m_IdealActivity = ACT_IDLE;
		}
		break;
	default:
		CBaseMonster_RunTask(e, t);
	}
}

void CHeadCrab_LeapTouch(int e, int other)
{
	if (!ent_priv(other) || !CBase(other)->takedamage)
		return;
	if (Classify(other) == Classify(e))
		return;
	if (!(ent_cv(e)->flags & FL_ONGROUND))
	{
		EMIT_SOUND_DYN(e, CHAN_WEAPON, RANDOM_SOUND_ARRAY(hcBite), 1.0f, ATTN_IDLE, 0, 100);
		TakeDamage(other, e, e, mon_skill(sk_headcrabDmgBite), DMG_SLASH);
	}
	SetTouch(e, FN_NONE);
}

static void HC_IdleSound(int e)
{
	EMIT_SOUND_DYN(e, CHAN_VOICE, RANDOM_SOUND_ARRAY(hcIdle), 1.0f, ATTN_IDLE, 0, 100);
}
static void HC_AlertSound(int e)
{
	EMIT_SOUND_DYN(e, CHAN_VOICE, RANDOM_SOUND_ARRAY(hcAlert), 1.0f, ATTN_IDLE, 0, 100);
}
static void HC_PainSound(int e)
{
	EMIT_SOUND_DYN(e, CHAN_VOICE, RANDOM_SOUND_ARRAY(hcPain), 1.0f, ATTN_IDLE, 0, 100);
}
static void HC_DeathSound(int e)
{
	EMIT_SOUND_DYN(e, CHAN_VOICE, RANDOM_SOUND_ARRAY(hcDeath), 1.0f, ATTN_IDLE, 0, 100);
}

static void HC_PrescheduleThink(int e)
{
	if (MON(e)->m_MonsterState == MONSTERSTATE_COMBAT && RANDOM_FLOAT(0, 5) < 0.1f)
		HC_IdleSound(e);
}

static void HC_StartTask(int e, const Task_t *t)
{
	CBaseMonster *m = MON(e);

	m->m_iTaskStatus = TASKSTATUS_RUNNING;
	if (t->iTask == TASK_RANGE_ATTACK1)
	{
		EMIT_SOUND_DYN(e, CHAN_WEAPON, res_def(hcAttack[0]), 1.0f, ATTN_IDLE, 0, 100);
		m->m_IdealActivity = ACT_RANGE_ATTACK1;
		SetTouch(e, FN_CHeadCrab_LeapTouch);
		return;
	}
	CBaseMonster_StartTask(e, t);
}

static int HC_CheckRangeAttack1(int e, float dot, float dist)
{
	return (ent_cv(e)->flags & FL_ONGROUND) && dist <= 256 && dot >= 0.65f;
}

static int NoAttack(int e, float dot, float dist) { (void)e; (void)dot; (void)dist; return 0; }

static int HC_TakeDamage(int e, int inflictor, int attacker, float dmg, int bits)
{
	if (bits & DMG_ACID)
		dmg = 0;
	return CBaseMonster_TakeDamage(e, inflictor, attacker, dmg, bits);
}

static const Schedule_t *HC_GetScheduleOfType(int e, int type)
{
	if (type == SCHED_RANGE_ATTACK1)
		return &slHCRangeAttack1[0];
	return CBaseMonster_GetScheduleOfType(e, type);
}

static int CHeadCrab_Spawn(int e, gid_t16 gid)
{
	CBaseMonster	*m = Mon_SpawnKeys(e, gid);
	static const float mn[3] = { -12, -12, 0 }, mx[3] = { 12, 12, 24 };

	PRECACHE_SOUND_ARRAY(hcIdle);
	PRECACHE_SOUND_ARRAY(hcAlert);
	PRECACHE_SOUND_ARRAY(hcPain);
	PRECACHE_SOUND_ARRAY(hcAttack);
	PRECACHE_SOUND_ARRAY(hcDeath);
	PRECACHE_SOUND_ARRAY(hcBite);
	PRECACHE(RESID(MON_HEADCRAB_MDL));
	ent_set_model(e, RESID(MON_HEADCRAB_MDL));
	ent_set_size(e, mn, mx);
	ent_set_solid(e, SOLID_SLIDEBOX);
	ent_set_movetype(e, MOVETYPE_STEP);
	m->m_bloodColor = BLOOD_COLOR_GREEN;
	ent_rnd(e)->effects = 0;
	m->health = mon_skill(sk_headcrabHealth);
	VecSet(m->view_ofs, 0, 0, 20);
	ent_phys(e)->yaw_speed = 5;
	m->m_flFieldOfView = 0.5f;
	m->m_MonsterState = MONSTERSTATE_NONE;
	Mon_MonsterInit(e);
	return 0;
}

static const game_vt_t hc_vt = {
	MON_GAME_VT_BASE,
	.TraceAttack = CBaseMonster_TraceAttack,
	.Killed = CBaseMonster_Killed,
	.IRelationship = CBaseMonster_IRelationship,
	.Classify = HC_Classify,
	.TakeDamage = HC_TakeDamage,
};

/* game_vt_t.BodyTarget is set from the base macro; the headcrab overrides it */
static const mon_vt_t hc_mvt = {
	.RunTask = HC_RunTask,
	.StartTask = HC_StartTask,
	.SetYawSpeed = HC_SetYawSpeed,
	.BodyTarget = HC_BodyTarget,
	.PainSound = HC_PainSound,
	.DeathSound = HC_DeathSound,
	.IdleSound = HC_IdleSound,
	.AlertSound = HC_AlertSound,
	.PrescheduleThink = HC_PrescheduleThink,
	.HandleAnimEvent = HC_HandleAnimEvent,
	.CheckRangeAttack1 = HC_CheckRangeAttack1,
	.CheckRangeAttack2 = NoAttack,
	.GetScheduleOfType = HC_GetScheduleOfType,
};

const game_class_t gc_MONSTER_HEADCRAB = {
	.size = sizeof(CBaseMonster),
	.Spawn = CHeadCrab_Spawn,
	.vt = &hc_vt,
	.ext = &hc_mvt,
};

/* ---- monster_zombie (CZombie) ------------------------------------------------------ */

#define ZOMBIE_AE_ATTACK_RIGHT	0x01
#define ZOMBIE_AE_ATTACK_LEFT	0x02
#define ZOMBIE_AE_ATTACK_BOTH	0x03
#define ZOMBIE_FLINCH_DELAY	2

static const float sk_zombieHealth[3] = { 50, 50, 100 };
static const float sk_zombieDmgOneSlash[3] = { 10, 20, 20 };
static const float sk_zombieDmgBothSlash[3] = { 25, 40, 40 };

typedef struct {
	CBASEENTITY_FIELDS
	CBASEDELAY_FIELDS
	CBASEANIMATING_FIELDS
	CBASETOGGLE_FIELDS
	CBASEMONSTER_FIELDS
	float		m_flNextFlinch;
} CZombie;

static const int16_t zHit[] = { R_ZO_CLAW_STRIKE1, R_ZO_CLAW_STRIKE2, R_ZO_CLAW_STRIKE3 };
static const int16_t zMiss[] = { R_ZO_CLAW_MISS1, R_MON_CLAW_MISS2 };
static const int16_t zAttack[] = { R_ZO_ATTACK1, R_ZO_ATTACK2 };
static const int16_t zIdle[] = { R_ZO_IDLE1, R_ZO_IDLE2, R_ZO_IDLE3, R_ZO_IDLE4 };
static const int16_t zAlert[] = { R_ZO_ALERT10, R_ZO_ALERT20, R_ZO_ALERT30 };
static const int16_t zPain[] = { R_ZO_PAIN1, R_ZO_PAIN2 };

static int Z_Classify(int e) { (void)e; return CLASS_ALIEN_MONSTER; }
static void Z_SetYawSpeed(int e) { ent_phys(e)->yaw_speed = 120; }

static void Z_PainSound(int e)
{
	int pitch = 95 + RANDOM_LONG(0, 9);

	if (RANDOM_LONG(0, 5) < 2)
		EMIT_SOUND_DYN(e, CHAN_VOICE, RANDOM_SOUND_ARRAY(zPain), 1.0f, ATTN_NORM, 0, pitch);
}

static void Z_AlertSound(int e)
{
	int pitch = 95 + RANDOM_LONG(0, 9);

	EMIT_SOUND_DYN(e, CHAN_VOICE, RANDOM_SOUND_ARRAY(zAlert), 1.0f, ATTN_NORM, 0, pitch);
}

static void Z_IdleSound(int e)
{
	(void)RANDOM_LONG(0, 9);	/* HLSDK draws the unused pitch first */
	EMIT_SOUND_DYN(e, CHAN_VOICE, RANDOM_SOUND_ARRAY(zIdle), 1.0f, ATTN_NORM, 0, 100 + RANDOM_LONG(-5, 5));
}

static void Z_AttackSound(int e)
{
	EMIT_SOUND_DYN(e, CHAN_VOICE, RANDOM_SOUND_ARRAY(zAttack), 1.0f, ATTN_NORM, 0, 100 + RANDOM_LONG(-5, 5));
}

static int Z_TakeDamage(int e, int inflictor, int attacker, float dmg, int bits)
{
	float d[3], c[3], v[3];

	if (bits == DMG_BULLET && inflictor >= 0)
	{
		VecAdd(ent_cv(inflictor)->absmin, ent_cv(inflictor)->absmax, c);
		VecScale(c, 0.5f, c);
		VecSub(ent_cv(e)->origin, c, d);
		VecNorm(d, d);
		VecMA(ent_mot(e)->velocity, Mon_DamageForce(e, dmg), d, v);
		ent_set_velocity(e, v);
		dmg *= 0.3f;
	}
	if (IsAlive(e))
		Z_PainSound(e);
	return CBaseMonster_TakeDamage(e, inflictor, attacker, dmg, bits);
}

static void z_hit(int e, const float *sk, float px, float pz, float side, float fwd)
{
	int	hurt = Mon_CheckTraceHullAttack(e, 70, (int)mon_skill(sk), DMG_SLASH);
	float	v[3];

	if (hurt >= 0)
	{
		if (ent_cv(hurt)->flags & (FL_MONSTER | FL_CLIENT))
		{
			Mon_PunchPlayer(hurt, px, pz);
			VecMA(ent_mot(hurt)->velocity, side, gpGlobals.v_right, v);
			VecMA(v, fwd, gpGlobals.v_forward, v);
			ent_set_velocity(hurt, v);
		}
		EMIT_SOUND_DYN(e, CHAN_WEAPON, RANDOM_SOUND_ARRAY(zHit), 1.0f, ATTN_NORM, 0, 100 + RANDOM_LONG(-5, 5));
	}
	else
		EMIT_SOUND_DYN(e, CHAN_WEAPON, RANDOM_SOUND_ARRAY(zMiss), 1.0f, ATTN_NORM, 0, 100 + RANDOM_LONG(-5, 5));
	if (RANDOM_LONG(0, 1))
		Z_AttackSound(e);
}

static void Z_HandleAnimEvent(int e, const MonsterEvent_t *ev)
{
	switch (ev->event)
	{
	case ZOMBIE_AE_ATTACK_RIGHT:
		z_hit(e, sk_zombieDmgOneSlash, 5, -18, -100, 0);
		break;
	case ZOMBIE_AE_ATTACK_LEFT:
		z_hit(e, sk_zombieDmgOneSlash, 5, 18, 100, 0);
		break;
	case ZOMBIE_AE_ATTACK_BOTH:
		z_hit(e, sk_zombieDmgBothSlash, 5, 0, 0, -100);
		break;
	default:
		CBaseMonster_HandleAnimEvent(e, ev);
	}
}

static int Z_IgnoreConditions(int e)
{
	CZombie	*z = (CZombie *)ent_priv(e);
	int	ign = CBaseMonster_IgnoreConditions(e);

	if (z->m_Activity == ACT_MELEE_ATTACK1 && z->m_flNextFlinch >= gtime())
		ign |= bits_COND_LIGHT_DAMAGE | bits_COND_HEAVY_DAMAGE;
	if ((z->m_Activity == ACT_SMALL_FLINCH || z->m_Activity == ACT_BIG_FLINCH) &&
		z->m_flNextFlinch < gtime())
		z->m_flNextFlinch = gtime() + ZOMBIE_FLINCH_DELAY;
	return ign;
}

static int CZombie_Spawn(int e, gid_t16 gid)
{
	CBaseMonster *m = Mon_SpawnKeys(e, gid);

	PRECACHE(RESID(MON_ZOMBIE_MDL));
	PRECACHE_SOUND_ARRAY(zHit);
	PRECACHE_SOUND_ARRAY(zMiss);
	PRECACHE_SOUND_ARRAY(zAttack);
	PRECACHE_SOUND_ARRAY(zIdle);
	PRECACHE_SOUND_ARRAY(zAlert);
	PRECACHE_SOUND_ARRAY(zPain);
	ent_set_model(e, RESID(MON_ZOMBIE_MDL));
	ent_set_size(e, VEC_HUMAN_HULL_MIN, VEC_HUMAN_HULL_MAX);
	ent_set_solid(e, SOLID_SLIDEBOX);
	ent_set_movetype(e, MOVETYPE_STEP);
	m->m_bloodColor = BLOOD_COLOR_GREEN;
	m->health = mon_skill(sk_zombieHealth);
	VecCopy(VEC_VIEW, m->view_ofs);
	m->m_flFieldOfView = 0.5f;
	m->m_MonsterState = MONSTERSTATE_NONE;
	m->m_afCapability = bits_CAP_DOORS_GROUP;
	Mon_MonsterInit(e);
	return 0;
}

static const game_vt_t z_vt = {
	MON_GAME_VT_BASE,
	.TraceAttack = CBaseMonster_TraceAttack,
	.Killed = CBaseMonster_Killed,
	.IRelationship = CBaseMonster_IRelationship,
	.Classify = Z_Classify,
	.TakeDamage = Z_TakeDamage,
};

static const mon_vt_t z_mvt = {
	.SetYawSpeed = Z_SetYawSpeed,
	.HandleAnimEvent = Z_HandleAnimEvent,
	.IgnoreConditions = Z_IgnoreConditions,
	.PainSound = Z_PainSound,
	.AlertSound = Z_AlertSound,
	.IdleSound = Z_IdleSound,
	.CheckRangeAttack1 = NoAttack,
	.CheckRangeAttack2 = NoAttack,
};

const game_class_t gc_MONSTER_ZOMBIE = {
	.size = sizeof(CZombie),
	.Spawn = CZombie_Spawn,
	.vt = &z_vt,
	.ext = &z_mvt,
};
