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
 * barney.c -- monster_barney (HLSDK dlls/barney.cpp CBarney). Its dead
 * pose class monster_barney_dead is not on the Uplink / Hazard maps.
 */
#include "engine/phys/player.h"
#include "game/hl/monster.h"

#define VOL_NORM		1.0f
#define BARNEY_AE_DRAW		2
#define BARNEY_AE_SHOOT		3
#define BARNEY_AE_HOLSTER	4
#define BARNEY_BODY_GUNHOLSTERED	0
#define BARNEY_BODY_GUNDRAWN		1
#define BARNEY_BODY_GUNGONE		2

static const float sk_barneyHealth[3] = { 35, 35, 35 };

typedef struct {
	CBASEENTITY_FIELDS
	CBASEDELAY_FIELDS
	CBASEANIMATING_FIELDS
	CBASETOGGLE_FIELDS
	CBASEMONSTER_FIELDS
	CTALKMONSTER_FIELDS
	int		m_fGunDrawn;
	float		m_painTime;
	float		m_checkAttackTime;
	int		m_lastAttackCheck;
	float		m_flPlayerDamage;
} CBarney;

#define BA(e)	((CBarney *)ent_priv(e))
#define T(t, d)	{ t, (float)(d) }

static const Task_t tlBaFollow[] = {
	T(TASK_MOVE_TO_TARGET_RANGE, 128), T(TASK_SET_SCHEDULE, SCHED_TARGET_FACE),
};
static const Schedule_t slBaFollow[] = {
	SCHED(tlBaFollow, bits_COND_NEW_ENEMY | bits_COND_LIGHT_DAMAGE | bits_COND_HEAVY_DAMAGE |
		bits_COND_HEAR_SOUND | bits_COND_PROVOKED, bits_SOUND_DANGER, "Follow")
};

static const Task_t tlBarneyEnemyDraw[] = {
	T(TASK_STOP_MOVING, 0), T(TASK_FACE_ENEMY, 0), T(TASK_PLAY_SEQUENCE_FACE_ENEMY, ACT_ARM),
};
static const Schedule_t slBarneyEnemyDraw[] = { SCHED(tlBarneyEnemyDraw, 0, 0, "Barney Enemy Draw") };

static const Task_t tlBaFaceTarget[] = {
	T(TASK_SET_ACTIVITY, ACT_IDLE), T(TASK_FACE_TARGET, 0), T(TASK_SET_ACTIVITY, ACT_IDLE),
	T(TASK_SET_SCHEDULE, SCHED_TARGET_CHASE),
};
static const Schedule_t slBaFaceTarget[] = {
	SCHED(tlBaFaceTarget, bits_COND_CLIENT_PUSH | bits_COND_NEW_ENEMY | bits_COND_LIGHT_DAMAGE |
		bits_COND_HEAVY_DAMAGE | bits_COND_HEAR_SOUND | bits_COND_PROVOKED, bits_SOUND_DANGER,
		"FaceTarget")
};

static const Task_t tlIdleBaStand[] = {
	T(TASK_STOP_MOVING, 0), T(TASK_SET_ACTIVITY, ACT_IDLE), T(TASK_WAIT, 2),
	T(TASK_TLK_HEADRESET, 0),
};
static const Schedule_t slIdleBaStand[] = {
	SCHED(tlIdleBaStand, bits_COND_NEW_ENEMY | bits_COND_LIGHT_DAMAGE | bits_COND_HEAVY_DAMAGE |
		bits_COND_HEAR_SOUND | bits_COND_SMELL | bits_COND_PROVOKED,
		bits_SOUND_COMBAT | bits_SOUND_DANGER | bits_SOUND_MEAT | bits_SOUND_CARCASS |
		bits_SOUND_GARBAGE, "IdleStand")
};

static void CBarney_RunTask(int e, const Task_t *t)
{
	int en;

	if (t->iTask == TASK_RANGE_ATTACK1)
	{
		en = EHGet(BA(e)->m_hEnemy);
		if (en >= 0 && IsPlayer(en))
			ent_rnd(e)->framerate = 1.5f;
	}
	CTalkMonster_RunTask(e, t);
}

static int CBarney_ISoundMask(int e)
{
	(void)e;
	return bits_SOUND_WORLD | bits_SOUND_COMBAT | bits_SOUND_CARCASS | bits_SOUND_MEAT |
		bits_SOUND_GARBAGE | bits_SOUND_DANGER | bits_SOUND_PLAYER;
}

static int CBarney_Classify(int e) { (void)e; return CLASS_PLAYER_ALLY; }

static void CBarney_AlertSound(int e)
{
	if (EHGet(BA(e)->m_hEnemy) >= 0 && Tlk_FOkToSpeak(e))
		Mon_PlaySentence(e, RESID(GRP_BA_ATTACK), RANDOM_FLOAT(2.8f, 3.2f), VOL_NORM, ATTN_IDLE);
}

static void CBarney_SetYawSpeed(int e)
{
	int ys;

	switch (MON(e)->m_Activity)
	{
	case ACT_RUN: ys = 90; break;
	default: ys = 70; break;
	}
	ent_phys(e)->yaw_speed = (float)ys;
}

static int CBarney_CheckRangeAttack1(int e, float dot, float dist)
{
	CBarney		*b = BA(e);
	col_trace_t	tr;
	float		so[3], st[3], bt[3];
	int		en = EHGet(b->m_hEnemy);

	if (dist <= 1024 && dot >= 0.5f)
	{
		if (gtime() > b->m_checkAttackTime && en >= 0)
		{
			VecCopy(ent_cv(e)->origin, so);
			so[2] += 55;
			BodyTarget(en, so, bt);
			VecSub(bt, ent_cv(en)->origin, st);
			VecAdd(st, b->m_vecEnemyLKP, st);
			UTIL_TraceLine(so, st, dont_ignore_monsters, e, &tr);
			b->m_checkAttackTime = gtime() + 1;
			b->m_lastAttackCheck = tr.frac == 1.0f || tr.ent == en;
			b->m_checkAttackTime = gtime() + 1.5f;
		}
		return b->m_lastAttackCheck;
	}
	return 0;
}

static void BarneyFirePistol(int e)
{
	CBarney	*b = BA(e);
	float	so[3], dir[3], ang[3];
	int	ps;

	UTIL_MakeVectors(ent_cv(e)->angles);
	VecCopy(ent_cv(e)->origin, so);
	so[2] += 55;
	Mon_ShootAtEnemy(e, so, dir);
	UTIL_VecToAngles(dir, ang);
	gs_set_blending(ent_rnd(e)->model, ent_rnd(e)->sequence, ent_rnd(e)->blending, 0, ang[0]);
	ent_rnd(e)->effects = EF_MUZZLEFLASH;
	FireBullets(e, 1, so, dir, VECTOR_CONE_2DEGREES, 1024, BULLET_MONSTER_9MM, 4, 0, -1);
	ps = RANDOM_LONG(0, 20);
	ps = ps > 10 ? 0 : ps - 5;
	EMIT_SOUND_DYN(e, CHAN_WEAPON, RESID(BA_ATTACK2), 1, ATTN_NORM, 0, 100 + ps);
	CSoundEnt_InsertSound(bits_SOUND_COMBAT, ent_cv(e)->origin, 384, 0.3f);
	b->m_cAmmoLoaded--;
}

static void CBarney_HandleAnimEvent(int e, const MonsterEvent_t *ev)
{
	switch (ev->event)
	{
	case BARNEY_AE_SHOOT:
		BarneyFirePistol(e);
		break;
	case BARNEY_AE_DRAW:
		ent_rnd(e)->body = BARNEY_BODY_GUNDRAWN;
		BA(e)->m_fGunDrawn = 1;
		break;
	case BARNEY_AE_HOLSTER:
		ent_rnd(e)->body = BARNEY_BODY_GUNHOLSTERED;
		BA(e)->m_fGunDrawn = 0;
		break;
	default:
		CTalkMonster_HandleAnimEvent(e, ev);
	}
}

static void CBarney_TalkInit(int e)
{
	CTalkMonster *t = TALK(e);

	Tlk_TalkInit(e);
	t->m_szGrp[TLK_ANSWER] = RESID(GRP_BA_ANSWER);
	t->m_szGrp[TLK_QUESTION] = RESID(GRP_BA_QUESTION);
	t->m_szGrp[TLK_IDLE] = RESID(GRP_BA_IDLE);
	t->m_szGrp[TLK_STARE] = RESID(GRP_BA_STARE);
	t->m_szGrp[TLK_USE] = RESID(GRP_BA_OK);
	t->m_szGrp[TLK_UNUSE] = RESID(GRP_BA_WAIT);
	t->m_szGrp[TLK_STOP] = RESID(GRP_BA_STOP);
	t->m_szGrp[TLK_NOSHOOT] = RESID(GRP_BA_SCARED);
	t->m_szGrp[TLK_HELLO] = RESID(GRP_BA_HELLO);
	t->m_szGrp[TLK_PLHURT1] = RESID(SEN_BA_CUREA);
	t->m_szGrp[TLK_PLHURT2] = RESID(SEN_BA_CUREB);
	t->m_szGrp[TLK_PLHURT3] = RESID(SEN_BA_CUREC);
	t->m_szGrp[TLK_PHELLO] = RES_NONE;
	t->m_szGrp[TLK_PIDLE] = RES_NONE;
	t->m_szGrp[TLK_PQUESTION] = RES_NONE;	/* "BA_PQUEST": no such group in sentences.txt */
	t->m_szGrp[TLK_SMELL] = RESID(GRP_BA_SMELL);
	t->m_szGrp[TLK_WOUND] = RESID(GRP_BA_WOUND);
	t->m_szGrp[TLK_MORTAL] = RESID(GRP_BA_MORTAL);
	t->m_voicePitch = 100;
}

/* IsFacing: the player's view (v_angle without pitch) within ~15 degrees */
static int IsFacing(int test, const float ref[3])
{
	float	dir[3], fwd[3], ang[3];

	VecSub(ref, ent_cv(test)->origin, dir);
	dir[2] = 0;
	VecNorm(dir, dir);
	if (IsPlayer(test) && phy_player())
		VecCopy(phy_player()->view_angles, ang);
	else
		VecCopy(ent_cv(test)->angles, ang);
	ang[0] = 0;
	AngleVectors(ang, fwd, 0, 0);
	return VecDot(fwd, dir) > 0.96f;
}

static int CBarney_TakeDamage(int e, int inflictor, int attacker, float dmg, int bits)
{
	CBarney	*b = BA(e);
	int	ret = CTalkMonster_TakeDamage(e, inflictor, attacker, dmg, bits), en;

	if (!ent_is_live(e) || !IsAlive(e) || b->deadflag == DEAD_DYING)
		return ret;
	if (b->m_MonsterState != MONSTERSTATE_PRONE && attacker >= 0 && (ent_cv(attacker)->flags & FL_CLIENT))
	{
		b->m_flPlayerDamage += dmg;
		en = EHGet(b->m_hEnemy);
		if (en < 0)
		{
			if ((b->m_afMemory & bits_MEMORY_SUSPICIOUS) || IsFacing(attacker, ent_cv(e)->origin))
			{
				Mon_PlaySentence(e, RESID(GRP_BA_MAD), 4, VOL_NORM, ATTN_NORM);
				Mon_Remember((CBaseMonster *)b, bits_MEMORY_PROVOKED);
				Mon_StopFollowing(e, 1);
			}
			else
			{
				Mon_PlaySentence(e, RESID(GRP_BA_SHOT), 4, VOL_NORM, ATTN_NORM);
				Mon_Remember((CBaseMonster *)b, bits_MEMORY_SUSPICIOUS);
			}
		}
		else if (!IsPlayer(en) && b->deadflag == DEAD_NO)
			Mon_PlaySentence(e, RESID(GRP_BA_SHOT), 4, VOL_NORM, ATTN_NORM);
	}
	return ret;
}

static void CBarney_PainSound(int e)
{
	static const int16_t s[3] = { R_BA_PAIN1, R_BA_PAIN2, R_BA_PAIN3 };
	CBarney *b = BA(e);

	if (gtime() < b->m_painTime)
		return;
	b->m_painTime = gtime() + RANDOM_FLOAT(0.5f, 0.75f);
	EMIT_SOUND_DYN(e, CHAN_VOICE, res_def(s[RANDOM_LONG(0, 2)]), 1, ATTN_NORM, 0, Tlk_GetVoicePitch(e));
}

static void CBarney_DeathSound(int e)
{
	static const int16_t s[3] = { R_BA_DIE1, R_BA_DIE2, R_BA_DIE3 };

	EMIT_SOUND_DYN(e, CHAN_VOICE, res_def(s[RANDOM_LONG(0, 2)]), 1, ATTN_NORM, 0, Tlk_GetVoicePitch(e));
}

static void CBarney_TraceAttack(int e, int attacker, float dmg, const float dir[3],
	const col_trace_t *tr, int bits)
{
	col_trace_t t = *tr;

	switch (t.hitgroup)
	{
	case HITGROUP_CHEST:
	case HITGROUP_STOMACH:
		if (bits & (DMG_BULLET | DMG_SLASH | DMG_BLAST))
			dmg = dmg / 2;
		break;
	case 10:
		if (bits & (DMG_BULLET | DMG_SLASH | DMG_CLUB))
		{
			dmg -= 20;
			if (dmg <= 0)
			{
				UTIL_Ricochet(t.end, 1.0f);
				dmg = 0.01f;
			}
		}
		t.hitgroup = HITGROUP_HEAD;
		break;
	}
	CBaseMonster_TraceAttack(e, attacker, dmg, dir, &t, bits);
}

static void CBarney_Killed(int e, int attacker, int iGib)
{
	float pos[3], ang[3] = { 0, 0, 0 };

	if (ent_rnd(e)->body < BARNEY_BODY_GUNGONE)
	{
		ent_rnd(e)->body = BARNEY_BODY_GUNGONE;
		gs_attachment(e, 0, pos);
		Mon_DropItem(e, CLS_WEAPON_9MMHANDGUN, pos, ang);
	}
	SetUse(e, FN_NONE);
	CTalkMonster_Killed(e, attacker, iGib);
}

static const Schedule_t *CBarney_GetScheduleOfType(int e, int type)
{
	const Schedule_t *ps;

	switch (type)
	{
	case SCHED_ARM_WEAPON:
		if (EHGet(BA(e)->m_hEnemy) >= 0)
			return slBarneyEnemyDraw;
		break;
	case SCHED_TARGET_FACE:
		ps = CTalkMonster_GetScheduleOfType(e, type);
		return ps == slIdleStand ? slBaFaceTarget : ps;
	case SCHED_TARGET_CHASE:
		return slBaFollow;
	case SCHED_IDLE_STAND:
		ps = CTalkMonster_GetScheduleOfType(e, type);
		return ps == slIdleStand ? slIdleBaStand : ps;
	}
	return CTalkMonster_GetScheduleOfType(e, type);
}

static const Schedule_t *CBarney_GetSchedule(int e)
{
	CBarney		*b = BA(e);
	CBaseMonster	*m = (CBaseMonster *)b;
	CSound		*s;
	int		tg;

	if (Mon_HasConditions(m, bits_COND_HEAR_SOUND))
	{
		s = Mon_PBestSound(e);
		if (s && (s->m_iType & bits_SOUND_DANGER))
			return Mon_GetScheduleOfType(e, SCHED_TAKE_COVER_FROM_BEST_SOUND);
	}
	if (Mon_HasConditions(m, bits_COND_ENEMY_DEAD) && Tlk_FOkToSpeak(e))
		Mon_PlaySentence(e, RESID(GRP_BA_KILL), 4, VOL_NORM, ATTN_NORM);
	switch (m->m_MonsterState)
	{
	case MONSTERSTATE_COMBAT:
		if (Mon_HasConditions(m, bits_COND_ENEMY_DEAD))
			return CBaseMonster_GetSchedule(e);
		if (Mon_HasConditions(m, bits_COND_NEW_ENEMY) && Mon_HasConditions(m, bits_COND_LIGHT_DAMAGE))
			return Mon_GetScheduleOfType(e, SCHED_SMALL_FLINCH);
		if (!b->m_fGunDrawn)
			return Mon_GetScheduleOfType(e, SCHED_ARM_WEAPON);
		if (Mon_HasConditions(m, bits_COND_HEAVY_DAMAGE))
			return Mon_GetScheduleOfType(e, SCHED_TAKE_COVER_FROM_ENEMY);
		break;
	case MONSTERSTATE_ALERT:
	case MONSTERSTATE_IDLE:
		if (Mon_HasConditions(m, bits_COND_LIGHT_DAMAGE | bits_COND_HEAVY_DAMAGE))
			return Mon_GetScheduleOfType(e, SCHED_SMALL_FLINCH);
		if (EHGet(b->m_hEnemy) < 0 && Tlk_IsFollowing(e))
		{
			tg = EHGet(b->m_hTargetEnt);
			if (!IsAlive(tg))
			{
				Mon_StopFollowing(e, 0);
				break;
			}
			if (Mon_HasConditions(m, bits_COND_CLIENT_PUSH))
				return Mon_GetScheduleOfType(e, SCHED_MOVE_AWAY_FOLLOW);
			return Mon_GetScheduleOfType(e, SCHED_TARGET_FACE);
		}
		if (Mon_HasConditions(m, bits_COND_CLIENT_PUSH))
			return Mon_GetScheduleOfType(e, SCHED_MOVE_AWAY);
		Tlk_TrySmellTalk(e);
		break;
	}
	return CBaseMonster_GetSchedule(e);
}

static void CBarney_DeclineFollowing(int e)
{
	Mon_PlaySentence(e, RESID(GRP_BA_POK), 2, VOL_NORM, ATTN_NORM);
}

static int CBarney_ObjectCaps(int e) { (void)e; return FCAP_ACROSS_TRANSITION | FCAP_IMPULSE_USE; }

static int CBarney_Spawn(int e, gid_t16 gid)
{
	CBarney		*b;
	CBaseMonster	*m = Mon_SpawnKeys(e, gid);

	b = (CBarney *)m;
	Tlk_KeyValue(e, gid);
	PRECACHE(RESID(MON_BARNEY_MDL));
	PRECACHE(RESID(BA_ATTACK1));
	PRECACHE(RESID(BA_ATTACK2));
	CBarney_TalkInit(e);
	Tlk_Precache(e);
	ent_set_model(e, RESID(MON_BARNEY_MDL));
	ent_set_size(e, VEC_HUMAN_HULL_MIN, VEC_HUMAN_HULL_MAX);
	ent_set_solid(e, SOLID_SLIDEBOX);
	ent_set_movetype(e, MOVETYPE_STEP);
	m->m_bloodColor = BLOOD_COLOR_RED;
	m->health = mon_skill(sk_barneyHealth);
	VecSet(m->view_ofs, 0, 0, 50);
	m->m_flFieldOfView = VIEW_FIELD_WIDE;
	m->m_MonsterState = MONSTERSTATE_NONE;
	ent_rnd(e)->body = 0;
	b->m_fGunDrawn = 0;
	m->m_afCapability = bits_CAP_HEAR | bits_CAP_TURN_HEAD | bits_CAP_DOORS_GROUP;
	Mon_MonsterInit(e);
	SetUse(e, FN_CTalkMonster_FollowerUse);
	SetTouch(e, FN_CTalkMonster_Touch);	/* CTalkMonster::Touch override */
	return 0;
}

static const game_vt_t ba_vt = {
	MON_GAME_VT_BASE,
	.ObjectCaps = CBarney_ObjectCaps,
	.Classify = CBarney_Classify,
	.TakeDamage = CBarney_TakeDamage,
	.TraceAttack = CBarney_TraceAttack,
	.Killed = CBarney_Killed,
	.IRelationship = CTalkMonster_IRelationship,
};

static const mon_vt_t ba_mvt = {
	.SetYawSpeed = CBarney_SetYawSpeed,
	.ISoundMask = CBarney_ISoundMask,
	.AlertSound = CBarney_AlertSound,
	.HandleAnimEvent = CBarney_HandleAnimEvent,
	.RunTask = CBarney_RunTask,
	.StartTask = CTalkMonster_StartTask,
	.CheckRangeAttack1 = CBarney_CheckRangeAttack1,
	.DeclineFollowing = CBarney_DeclineFollowing,
	.GetScheduleOfType = CBarney_GetScheduleOfType,
	.GetSchedule = CBarney_GetSchedule,
	.DeathSound = CBarney_DeathSound,
	.PainSound = CBarney_PainSound,
	.TalkInit = CBarney_TalkInit,
	.SetActivity = CTalkMonster_SetActivity,
	.CanPlaySentence = CTalkMonster_CanPlaySentence,
	.PlaySentence = CTalkMonster_PlaySentence,
	.PlayScriptedSentence = CTalkMonster_PlayScriptedSentence,
	.PrescheduleThink = CTalkMonster_PrescheduleThink,
	.StopFollowing = CTalkMonster_StopFollowing,
	.SetAnswerQuestion = CTalkMonster_SetAnswerQuestion,
	.talk = 1,
};

const game_class_t gc_MONSTER_BARNEY = {
	.size = sizeof(CBarney),
	.Spawn = CBarney_Spawn,
	.vt = &ba_vt,
	.ext = &ba_mvt,
};
