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
 * generic.c -- monster_generic (HLSDK dlls/genericmonster.cpp) and
 * monster_gman (dlls/gman.cpp).
 */
#include "game/hl/monster.h"

#define SF_GENERICMONSTER_NOTSOLID	4

static const float VEC_HULL_MIN[3] = { -16, -16, -36 };
static const float VEC_HULL_MAX[3] = { 16, 16, 36 };

static int Generic_Classify(int e) { (void)e; return CLASS_PLAYER_ALLY; }
static void YawSpeed90(int e) { ent_phys(e)->yaw_speed = 90; }
static int NoSounds(int e) { (void)e; return 0; }

static int CGenericMonster_Spawn(int e, gid_t16 gid)
{
	CBaseMonster	*m = Mon_SpawnKeys(e, gid);
	res_t		model = m->model;

	PRECACHE(model);
	ent_set_model(e, model);
	if (model == RESID(MON_PLAYER_MDL) || model == RESID(MON_HOLO_MDL))
		ent_set_size(e, VEC_HULL_MIN, VEC_HULL_MAX);
	else
		ent_set_size(e, VEC_HUMAN_HULL_MIN, VEC_HUMAN_HULL_MAX);
	ent_set_solid(e, SOLID_SLIDEBOX);
	ent_set_movetype(e, MOVETYPE_STEP);
	m->m_bloodColor = BLOOD_COLOR_RED;
	m->health = 8;
	m->m_flFieldOfView = 0.5f;
	m->m_MonsterState = MONSTERSTATE_NONE;
	Mon_MonsterInit(e);
	if (m->spawnflags & SF_GENERICMONSTER_NOTSOLID)
	{
		ent_set_solid(e, SOLID_NOT);
		m->takedamage = DAMAGE_NO;
	}
	return 0;
}

static const game_vt_t generic_vt = {
	MON_GAME_VT,
	.Classify = Generic_Classify,
};

static const mon_vt_t generic_mvt = {
	.SetYawSpeed = YawSpeed90,
	.ISoundMask = NoSounds,
};

const game_class_t gc_MONSTER_GENERIC = {
	.size = sizeof(CBaseMonster),
	.Spawn = CGenericMonster_Spawn,
	.vt = &generic_vt,
	.ext = &generic_mvt,
	.save = &sv_CBaseMonster,
};

/* ---- monster_gman (CGMan) --------------------------------------------------------- */

typedef struct {
	CBASEENTITY_FIELDS
	CBASEDELAY_FIELDS
	CBASEANIMATING_FIELDS
	CBASETOGGLE_FIELDS
	CBASEMONSTER_FIELDS
	ent_h		m_hPlayer;
	ent_h		m_hTalkTarget;
	float		m_flTalkTime;
} CGMan;

#define GMAN(e)	((CGMan *)ent_priv(e))

static int GMan_Classify(int e) { (void)e; return CLASS_NONE; }

static int CGMan_Spawn(int e, gid_t16 gid)
{
	CBaseMonster	*m = Mon_SpawnKeys(e, gid);
	CGMan		*g = (CGMan *)m;

	g->m_hPlayer = g->m_hTalkTarget = EH_NONE;
	PRECACHE(RESID(MON_GMAN_MDL));
	ent_set_model(e, RESID(MON_GMAN_MDL));
	ent_set_size(e, VEC_HUMAN_HULL_MIN, VEC_HUMAN_HULL_MAX);
	ent_set_solid(e, SOLID_SLIDEBOX);
	ent_set_movetype(e, MOVETYPE_STEP);
	m->m_bloodColor = DONT_BLEED;
	m->health = 100;
	m->m_flFieldOfView = 0.5f;
	m->m_MonsterState = MONSTERSTATE_NONE;
	Mon_MonsterInit(e);
	return 0;
}

static void CGMan_StartTask(int e, const Task_t *t)
{
	CGMan *g = GMAN(e);

	if (t->iTask == TASK_WAIT && EHGet(g->m_hPlayer) < 0 && ent_is_live(ENT_PLAYER))
		g->m_hPlayer = EHANDLE(ENT_PLAYER);
	CBaseMonster_StartTask(e, t);
}

static void head_at(int e, int target)
{
	float d[3], yaw;

	VecSub(ent_cv(target)->origin, ent_cv(e)->origin, d);
	yaw = Mon_VecToYaw(e, d) - ent_cv(e)->angles[1];
	if (yaw > 180)
		yaw -= 360;
	if (yaw < -180)
		yaw += 360;
	Mon_SetBoneController(e, 0, yaw);
}

static void CGMan_RunTask(int e, const Task_t *t)
{
	CGMan	*g = GMAN(e);
	int	tt = EHGet(g->m_hTalkTarget), pl = EHGet(g->m_hPlayer);

	if (t->iTask == TASK_WAIT)
	{
		if (g->m_flTalkTime > gtime() && tt >= 0)
			head_at(e, tt);
		else if (pl >= 0 && ent_rnd(e)->sequence == 0)
			head_at(e, pl);
		else
			Mon_SetBoneController(e, 0, 0);
	}
	else
		Mon_SetBoneController(e, 0, 0);
	CBaseMonster_RunTask(e, t);
}

static int CGMan_TakeDamage(int e, int inflictor, int attacker, float dmg, int bits)
{
	CBaseMonster *m = MON(e);

	(void)inflictor; (void)attacker; (void)bits;
	m->health = m->max_health / 2;
	if (dmg > 0)
		Mon_SetConditions(m, bits_COND_LIGHT_DAMAGE);
	if (dmg >= 20)
		Mon_SetConditions(m, bits_COND_HEAVY_DAMAGE);
	return 1;
}

static void CGMan_TraceAttack(int e, int attacker, float dmg, const float dir[3],
	const col_trace_t *tr, int bits)
{
	(void)dir;
	UTIL_Ricochet(tr->end, 1.0f);
	AddMultiDamage(attacker, e, dmg, bits);
}

static void CGMan_PlayScriptedSentence(int e, res_t s, float dur, float vol, float attn, int conc,
	int listener)
{
	CGMan *g = GMAN(e);

	(void)conc;
	Mon_PlaySentence(e, s, dur, vol, attn);
	g->m_flTalkTime = gtime() + dur;
	g->m_hTalkTarget = EHANDLE(listener);
}

static const game_vt_t gman_vt = {
	MON_GAME_VT_BASE,
	.Classify = GMan_Classify,
	.TakeDamage = CGMan_TakeDamage,
	.TraceAttack = CGMan_TraceAttack,
	.Killed = CBaseMonster_Killed,
	.IRelationship = CBaseMonster_IRelationship,
};

static const mon_vt_t gman_mvt = {
	.SetYawSpeed = YawSpeed90,
	.ISoundMask = NoSounds,
	.StartTask = CGMan_StartTask,
	.RunTask = CGMan_RunTask,
	.PlayScriptedSentence = CGMan_PlayScriptedSentence,
};

/* HLSDK CGMan::m_SaveData (+ m_hPlayer of our layout) */
static const sv_field_t f_CGMan[] = {
	SVF(CGMan, m_hPlayer, SV_EH, 0),
	SVF(CGMan, m_hTalkTarget, SV_EH, 0),
	SVF(CGMan, m_flTalkTime, SV_TIME, 0),
};
static const sv_table_t sv_CGMan = SV_TABLE(&sv_CBaseMonster, f_CGMan);

const game_class_t gc_MONSTER_GMAN = {
	.size = sizeof(CGMan),
	.Spawn = CGMan_Spawn,
	.vt = &gman_vt,
	.ext = &gman_mvt,
	.save = &sv_CGMan,
};
