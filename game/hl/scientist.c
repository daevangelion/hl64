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
 * scientist.c -- monster_scientist and monster_scientist_dead (HLSDK
 * dlls/scientist.cpp CScientist, CDeadScientist). monster_sitting_scientist
 * is not a class of the Uplink / Hazard maps.
 */
#include "game/hl/monster.h"

#define VOL_NORM		1.0f
#define NUM_SCIENTIST_HEADS	4
enum { HEAD_GLASSES = 0, HEAD_EINSTEIN = 1, HEAD_LUTHER = 2, HEAD_SLICK = 3 };

enum {
	SCHED_HIDE = LAST_TALKMONSTER_SCHEDULE + 1, SCHED_FEAR, SCHED_PANIC, SCHED_STARTLE,
	SCHED_TARGET_CHASE_SCARED, SCHED_TARGET_FACE_SCARED
};
enum {
	TASK_SAY_HEAL = LAST_TALKMONSTER_TASK + 1, TASK_HEAL, TASK_SAY_FEAR, TASK_RUN_PATH_SCARED,
	TASK_SCREAM, TASK_RANDOM_SCREAM, TASK_MOVE_TO_TARGET_RANGE_SCARED
};

#define SCIENTIST_AE_HEAL	1
#define SCIENTIST_AE_NEEDLEON	2
#define SCIENTIST_AE_NEEDLEOFF	3

static const float sk_scientistHealth[3] = { 20, 20, 20 };
static const float sk_scientistHeal[3] = { 25, 25, 25 };

typedef struct {
	CBASEENTITY_FIELDS
	CBASEDELAY_FIELDS
	CBASEANIMATING_FIELDS
	CBASETOGGLE_FIELDS
	CBASEMONSTER_FIELDS
	CTALKMONSTER_FIELDS
	float		m_painTime;
	float		m_healTime;
	float		m_fearTime;
} CScientist;

#define SCI(e)	((CScientist *)ent_priv(e))

#define T(t, d)	{ t, (float)(d) }

static const Task_t tlFollow[] = {
	T(TASK_SET_FAIL_SCHEDULE, SCHED_CANT_FOLLOW), T(TASK_MOVE_TO_TARGET_RANGE, 128),
};
static const Schedule_t slFollow[] = {
	SCHED(tlFollow, bits_COND_NEW_ENEMY | bits_COND_LIGHT_DAMAGE | bits_COND_HEAVY_DAMAGE |
		bits_COND_HEAR_SOUND, bits_SOUND_COMBAT | bits_SOUND_DANGER, "Follow")
};

static const Task_t tlFollowScared[] = {
	T(TASK_SET_FAIL_SCHEDULE, SCHED_TARGET_CHASE), T(TASK_MOVE_TO_TARGET_RANGE_SCARED, 128),
};
static const Schedule_t slFollowScared[] = {
	SCHED(tlFollowScared, bits_COND_NEW_ENEMY | bits_COND_HEAR_SOUND | bits_COND_LIGHT_DAMAGE |
		bits_COND_HEAVY_DAMAGE, bits_SOUND_DANGER, "FollowScared")
};

static const Task_t tlFaceTargetScared[] = {
	T(TASK_FACE_TARGET, 0), T(TASK_SET_ACTIVITY, ACT_CROUCHIDLE),
	T(TASK_SET_SCHEDULE, SCHED_TARGET_CHASE_SCARED),
};
static const Schedule_t slFaceTargetScared[] = {
	SCHED(tlFaceTargetScared, bits_COND_HEAR_SOUND | bits_COND_NEW_ENEMY, bits_SOUND_DANGER,
		"FaceTargetScared")
};

static const Task_t tlStopFollowing[] = { T(TASK_CANT_FOLLOW, 0) };
static const Schedule_t slStopFollowing[] = { SCHED(tlStopFollowing, 0, 0, "StopFollowing") };

static const Task_t tlHeal[] = {
	T(TASK_MOVE_TO_TARGET_RANGE, 50), T(TASK_SET_FAIL_SCHEDULE, SCHED_TARGET_CHASE),
	T(TASK_FACE_IDEAL, 0), T(TASK_SAY_HEAL, 0), T(TASK_PLAY_SEQUENCE_FACE_TARGET, ACT_ARM),
	T(TASK_HEAL, 0), T(TASK_PLAY_SEQUENCE_FACE_TARGET, ACT_DISARM),
};
static const Schedule_t slHeal[] = { SCHED(tlHeal, 0, 0, "Heal") };

static const Task_t tlFaceTarget[] = {
	T(TASK_STOP_MOVING, 0), T(TASK_FACE_TARGET, 0), T(TASK_SET_ACTIVITY, ACT_IDLE),
	T(TASK_SET_SCHEDULE, SCHED_TARGET_CHASE),
};
static const Schedule_t slFaceTarget[] = {
	SCHED(tlFaceTarget, bits_COND_CLIENT_PUSH | bits_COND_NEW_ENEMY | bits_COND_HEAR_SOUND,
		bits_SOUND_COMBAT | bits_SOUND_DANGER, "FaceTarget")
};

static const Task_t tlSciPanic[] = {
	T(TASK_STOP_MOVING, 0), T(TASK_FACE_ENEMY, 0), T(TASK_SCREAM, 0),
	T(TASK_PLAY_SEQUENCE_FACE_ENEMY, ACT_EXCITED), T(TASK_SET_ACTIVITY, ACT_IDLE),
};
static const Schedule_t slSciPanic[] = { SCHED(tlSciPanic, 0, 0, "SciPanic") };

static const Task_t tlIdleSciStand[] = {
	T(TASK_STOP_MOVING, 0), T(TASK_SET_ACTIVITY, ACT_IDLE), T(TASK_WAIT, 2),
	T(TASK_TLK_HEADRESET, 0),
};
static const Schedule_t slIdleSciStand[] = {
	SCHED(tlIdleSciStand, bits_COND_NEW_ENEMY | bits_COND_LIGHT_DAMAGE | bits_COND_HEAVY_DAMAGE |
		bits_COND_HEAR_SOUND | bits_COND_SMELL | bits_COND_CLIENT_PUSH | bits_COND_PROVOKED,
		bits_SOUND_COMBAT | bits_SOUND_DANGER | bits_SOUND_MEAT | bits_SOUND_CARCASS |
		bits_SOUND_GARBAGE, "IdleSciStand")
};

static const Task_t tlScientistCover[] = {
	T(TASK_SET_FAIL_SCHEDULE, SCHED_PANIC), T(TASK_STOP_MOVING, 0),
	T(TASK_FIND_COVER_FROM_ENEMY, 0), T(TASK_RUN_PATH_SCARED, 0), T(TASK_TURN_LEFT, 179),
	T(TASK_SET_SCHEDULE, SCHED_HIDE),
};
static const Schedule_t slScientistCover[] = {
	SCHED(tlScientistCover, bits_COND_NEW_ENEMY, 0, "ScientistCover")
};

static const Task_t tlScientistHide[] = {
	T(TASK_SET_FAIL_SCHEDULE, SCHED_PANIC), T(TASK_STOP_MOVING, 0),
	T(TASK_PLAY_SEQUENCE, ACT_CROUCH), T(TASK_SET_ACTIVITY, ACT_CROUCHIDLE),
	T(TASK_WAIT_RANDOM, 10.0),
};
static const Schedule_t slScientistHide[] = {
	SCHED(tlScientistHide, bits_COND_NEW_ENEMY | bits_COND_HEAR_SOUND | bits_COND_SEE_ENEMY |
		bits_COND_SEE_HATE | bits_COND_SEE_FEAR | bits_COND_SEE_DISLIKE, bits_SOUND_DANGER,
		"ScientistHide")
};

static const Task_t tlScientistStartle[] = {
	T(TASK_SET_FAIL_SCHEDULE, SCHED_PANIC), T(TASK_RANDOM_SCREAM, 0.3), T(TASK_STOP_MOVING, 0),
	T(TASK_PLAY_SEQUENCE_FACE_ENEMY, ACT_CROUCH), T(TASK_RANDOM_SCREAM, 0.1),
	T(TASK_PLAY_SEQUENCE_FACE_ENEMY, ACT_CROUCHIDLE), T(TASK_WAIT_RANDOM, 1.0),
};
static const Schedule_t slScientistStartle[] = {
	SCHED(tlScientistStartle, bits_COND_NEW_ENEMY | bits_COND_SEE_ENEMY | bits_COND_SEE_HATE |
		bits_COND_SEE_FEAR | bits_COND_SEE_DISLIKE, 0, "ScientistStartle")
};

static const Task_t tlFear[] = { T(TASK_STOP_MOVING, 0), T(TASK_FACE_ENEMY, 0), T(TASK_SAY_FEAR, 0) };
static const Schedule_t slFear[] = { SCHED(tlFear, bits_COND_NEW_ENEMY, 0, "Fear") };

/* saved games: append only (monster.h sched_list_t) */
SCHED_LIST(sched_scientist, slFollow, slFollowScared, slFaceTargetScared, slStopFollowing, slHeal,
	slFaceTarget, slSciPanic, slIdleSciStand, slScientistCover, slScientistHide,
	slScientistStartle, slFear);

/* ---- speech ----------------------------------------------------------------------- */

static void Scream(int e)
{
	CScientist *s = SCI(e);

	if (Tlk_FOkToSpeak(e))
	{
		Tlk_Talk(e, 10);
		s->m_hTalkTarget = s->m_hEnemy;
		Mon_PlaySentence(e, RESID(GRP_SC_SCREAM), RANDOM_FLOAT(3, 6), VOL_NORM, ATTN_NORM);
	}
}

static void CScientist_DeclineFollowing(int e)
{
	CScientist *s = SCI(e);

	Tlk_Talk(e, 10);
	s->m_hTalkTarget = s->m_hEnemy;
	Mon_PlaySentence(e, RESID(GRP_SC_POK), 2, VOL_NORM, ATTN_NORM);
}

static int CScientist_GetStoppedActivity(int e)
{
	if (EHGet(SCI(e)->m_hEnemy) >= 0)
		return ACT_EXCITED;
	return ACT_IDLE;
}

static int CanHeal(int e)
{
	CScientist	*s = SCI(e);
	int		t = EHGet(s->m_hTargetEnt);

	if (s->m_healTime > gtime() || t < 0 || !ent_priv(t) || CBase(t)->health > CBase(t)->max_health * 0.5f)
		return 0;
	return 1;
}

static void Heal(int e)
{
	CScientist	*s = SCI(e);
	int		t;
	float		tp[3];

	if (!CanHeal(e))
		return;
	t = EHGet(s->m_hTargetEnt);
	OriginIn(t, e, tp);
	if (mv_dist(tp, ent_cv(e)->origin) > 100)
		return;
	TakeHealth(t, mon_skill(sk_scientistHeal), DMG_GENERIC);
	s->m_healTime = gtime() + 60;
}

static void CScientist_StartTask(int e, const Task_t *t)
{
	CScientist	*s = SCI(e);
	CBaseMonster	*m = (CBaseMonster *)s;
	int		en, tg;
	float		tp[3];

	switch (t->iTask)
	{
	case TASK_SAY_HEAL:
		Tlk_Talk(e, 2);
		s->m_hTalkTarget = s->m_hTargetEnt;
		Mon_PlaySentence(e, RESID(GRP_SC_HEAL), 2, VOL_NORM, ATTN_IDLE);
		Mon_TaskComplete(m);
		break;
	case TASK_SCREAM:
		Scream(e);
		Mon_TaskComplete(m);
		break;
	case TASK_RANDOM_SCREAM:
		if (RANDOM_FLOAT(0, 1) < t->flData)
			Scream(e);
		Mon_TaskComplete(m);
		break;
	case TASK_SAY_FEAR:
		if (Tlk_FOkToSpeak(e))
		{
			Tlk_Talk(e, 2);
			s->m_hTalkTarget = s->m_hEnemy;
			en = EHGet(s->m_hEnemy);
			if (en >= 0 && IsPlayer(en))
				Mon_PlaySentence(e, RESID(GRP_SC_PLFEAR), 5, VOL_NORM, ATTN_NORM);
			else
				Mon_PlaySentence(e, RESID(GRP_SC_FEAR), 5, VOL_NORM, ATTN_NORM);
		}
		Mon_TaskComplete(m);
		break;
	case TASK_HEAL:
		m->m_IdealActivity = ACT_MELEE_ATTACK1;
		break;
	case TASK_RUN_PATH_SCARED:
		m->m_movementActivity = ACT_RUN_SCARED;
		break;
	case TASK_MOVE_TO_TARGET_RANGE_SCARED:
		tg = EHGet(s->m_hTargetEnt);
		if (tg >= 0)
			OriginIn(tg, e, tp);
		if (tg < 0)
			Mon_TaskFail(m);
		else if (mv_dist(tp, ent_cv(e)->origin) < 1)
			Mon_TaskComplete(m);
		else
		{
			VecCopy(tp, m->m_vecMoveGoal);
			if (!Mon_MoveToTarget(e, ACT_WALK_SCARED, 0.5f))
				Mon_TaskFail(m);
		}
		break;
	default:
		CTalkMonster_StartTask(e, t);
		break;
	}
}

static void CScientist_RunTask(int e, const Task_t *t)
{
	CScientist	*s = SCI(e);
	CBaseMonster	*m = (CBaseMonster *)s;
	int		tg;
	float		d[3], dist, tp[3];

	switch (t->iTask)
	{
	case TASK_RUN_PATH_SCARED:
		if (Mon_MovementIsComplete(m))
			Mon_TaskComplete(m);
		if (RANDOM_LONG(0, 31) < 8)
			Scream(e);
		break;
	case TASK_MOVE_TO_TARGET_RANGE_SCARED:
		if (RANDOM_LONG(0, 63) < 8)
			Scream(e);
		tg = EHGet(s->m_hTargetEnt);
		if (EHGet(s->m_hEnemy) < 0 || tg < 0)
			Mon_TaskFail(m);
		else
		{
			OriginIn(tg, e, tp);
			dist = mv_dist2d(m->m_vecMoveGoal, ent_cv(e)->origin);
			if (dist < t->flData || mv_dist(m->m_vecMoveGoal, tp) > t->flData * 0.5f)
			{
				VecCopy(tp, m->m_vecMoveGoal);
				dist = mv_dist2d(m->m_vecMoveGoal, ent_cv(e)->origin);
				Mon_FRefreshRoute(e);
			}
			if (dist < t->flData)
			{
				Mon_TaskComplete(m);
				Mon_RouteClear(e);
			}
			else if (dist < 190 && m->m_movementActivity != ACT_WALK_SCARED)
				m->m_movementActivity = ACT_WALK_SCARED;
			else if (dist >= 270 && m->m_movementActivity != ACT_RUN_SCARED)
				m->m_movementActivity = ACT_RUN_SCARED;
		}
		break;
	case TASK_HEAL:
		if (m->anim.m_fSequenceFinished)
			Mon_TaskComplete(m);
		else
		{
			if (Tlk_TargetDistance(e) > 90)
				Mon_TaskComplete(m);
			tg = EHGet(s->m_hTargetEnt);
			if (tg >= 0)
			{
				OriginIn(tg, e, d);
				VecSub(d, ent_cv(e)->origin, d);
				ent_phys(e)->ideal_yaw = UTIL_VecToYaw(d);
			}
			Mon_ChangeYaw(e, (int)ent_phys(e)->yaw_speed);
		}
		break;
	default:
		CTalkMonster_RunTask(e, t);
		break;
	}
}

static int CScientist_Classify(int e) { (void)e; return CLASS_HUMAN_PASSIVE; }

static void CScientist_SetYawSpeed(int e)
{
	int ys = 90;

	switch (MON(e)->m_Activity)
	{
	case ACT_IDLE: ys = 120; break;
	case ACT_WALK: ys = 180; break;
	case ACT_RUN: ys = 150; break;
	case ACT_TURN_LEFT:
	case ACT_TURN_RIGHT: ys = 120; break;
	}
	ent_phys(e)->yaw_speed = (float)ys;
}

static void CScientist_HandleAnimEvent(int e, const MonsterEvent_t *ev)
{
	ent_rnd_t *r = ent_rnd(e);

	switch (ev->event)
	{
	case SCIENTIST_AE_HEAL:
		Heal(e);
		break;
	case SCIENTIST_AE_NEEDLEON:
		r->body = (uint8_t)((r->body % NUM_SCIENTIST_HEADS) + NUM_SCIENTIST_HEADS * 1);
		break;
	case SCIENTIST_AE_NEEDLEOFF:
		r->body = (uint8_t)((r->body % NUM_SCIENTIST_HEADS) + NUM_SCIENTIST_HEADS * 0);
		break;
	default:
		CTalkMonster_HandleAnimEvent(e, ev);
	}
}

static void CScientist_TalkInit(int e)
{
	CTalkMonster *t = TALK(e);

	Tlk_TalkInit(e);
	Tlk_SetFriends(CLS_MONSTER_SCIENTIST, CLS_NONE /* monster_sitting_scientist */, CLS_MONSTER_BARNEY);
	t->m_szGrp[TLK_ANSWER] = RESID(GRP_SC_ANSWER);
	t->m_szGrp[TLK_QUESTION] = RESID(GRP_SC_QUESTION);
	t->m_szGrp[TLK_IDLE] = RESID(GRP_SC_IDLE);
	t->m_szGrp[TLK_STARE] = RESID(GRP_SC_STARE);
	t->m_szGrp[TLK_USE] = RESID(GRP_SC_OK);
	t->m_szGrp[TLK_UNUSE] = RESID(GRP_SC_WAIT);
	t->m_szGrp[TLK_STOP] = RESID(GRP_SC_STOP);
	t->m_szGrp[TLK_NOSHOOT] = RESID(GRP_SC_SCARED);
	t->m_szGrp[TLK_HELLO] = RESID(GRP_SC_HELLO);
	t->m_szGrp[TLK_PLHURT1] = RESID(SEN_SC_CUREA);
	t->m_szGrp[TLK_PLHURT2] = RESID(SEN_SC_CUREB);
	t->m_szGrp[TLK_PLHURT3] = RESID(SEN_SC_CUREC);
	t->m_szGrp[TLK_PHELLO] = RESID(GRP_SC_PHELLO);
	t->m_szGrp[TLK_PIDLE] = RESID(GRP_SC_PIDLE);
	t->m_szGrp[TLK_PQUESTION] = RESID(GRP_SC_PQUEST);
	t->m_szGrp[TLK_SMELL] = RESID(GRP_SC_SMELL);
	t->m_szGrp[TLK_WOUND] = RESID(GRP_SC_WOUND);
	t->m_szGrp[TLK_MORTAL] = RESID(GRP_SC_MORTAL);
	switch (ent_rnd(e)->body % 3)
	{
	default:
	case HEAD_GLASSES: t->m_voicePitch = 105; break;
	case HEAD_EINSTEIN: t->m_voicePitch = 100; break;
	case HEAD_LUTHER: t->m_voicePitch = 95; break;
	}
}

static int CScientist_TakeDamage(int e, int inflictor, int attacker, float dmg, int bits)
{
	if (inflictor >= 0 && (ent_cv(inflictor)->flags & FL_CLIENT))
	{
		Mon_Remember(MON(e), bits_MEMORY_PROVOKED);
		Mon_StopFollowing(e, 1);
	}
	return CTalkMonster_TakeDamage(e, inflictor, attacker, dmg, bits);
}

static int CScientist_ISoundMask(int e)
{
	(void)e;
	return bits_SOUND_WORLD | bits_SOUND_COMBAT | bits_SOUND_DANGER | bits_SOUND_PLAYER;
}

static void CScientist_PainSound(int e)
{
	CScientist *s = SCI(e);
	static const int16_t pain[5] = { R_SCI_PAIN1, R_SCI_PAIN2, R_SCI_PAIN3, R_SCI_PAIN4, R_SCI_PAIN5 };
	res_t snd;

	if (gtime() < s->m_painTime)
		return;
	s->m_painTime = gtime() + RANDOM_FLOAT(0.5f, 0.75f);
	/* the sample before the pitch: argument order is unspecified (K10) */
	snd = res_def(pain[RANDOM_LONG(0, 4)]);
	EMIT_SOUND_DYN(e, CHAN_VOICE, snd, 1, ATTN_NORM, 0, Tlk_GetVoicePitch(e));
}

static void CScientist_Killed(int e, int attacker, int iGib)
{
	SetUse(e, FN_NONE);
	CTalkMonster_Killed(e, attacker, iGib);
}

static void CScientist_SetActivity(int e, int act)
{
	if (Mon_LookupActivity(e, act) == ACTIVITY_NOT_AVAILABLE)
		act = ACT_IDLE;
	CTalkMonster_SetActivity(e, act);
}

static const Schedule_t *CScientist_GetScheduleOfType(int e, int type)
{
	const Schedule_t *ps;

	switch (type)
	{
	case SCHED_TARGET_FACE:
		ps = CTalkMonster_GetScheduleOfType(e, type);
		return ps == slIdleStand ? slFaceTarget : ps;
	case SCHED_TARGET_CHASE:
		return slFollow;
	case SCHED_CANT_FOLLOW:
		return slStopFollowing;
	case SCHED_PANIC:
		return slSciPanic;
	case SCHED_TARGET_CHASE_SCARED:
		return slFollowScared;
	case SCHED_TARGET_FACE_SCARED:
		return slFaceTargetScared;
	case SCHED_IDLE_STAND:
		ps = CTalkMonster_GetScheduleOfType(e, type);
		return ps == slIdleStand ? slIdleSciStand : ps;
	case SCHED_HIDE:
		return slScientistHide;
	case SCHED_STARTLE:
		return slScientistStartle;
	case SCHED_FEAR:
		return slFear;
	}
	return CTalkMonster_GetScheduleOfType(e, type);
}

static int DisregardEnemy(int e, int en)
{
	return !IsAlive(en) || (gtime() - SCI(e)->m_fearTime) > 15;
}

static const Schedule_t *CScientist_GetSchedule(int e)
{
	CScientist	*s = SCI(e);
	CBaseMonster	*m = (CBaseMonster *)s;
	int		en = EHGet(s->m_hEnemy), rel, tg;
	CSound		*snd;

	if (Mon_HasConditions(m, bits_COND_HEAR_SOUND))
	{
		snd = Mon_PBestSound(e);
		if (snd && (snd->m_iType & bits_SOUND_DANGER))
			return Mon_GetScheduleOfType(e, SCHED_TAKE_COVER_FROM_BEST_SOUND);
	}
	switch (m->m_MonsterState)
	{
	case MONSTERSTATE_ALERT:
	case MONSTERSTATE_IDLE:
		if (en >= 0)
		{
			if (Mon_HasConditions(m, bits_COND_SEE_ENEMY))
				s->m_fearTime = gtime();
			else if (DisregardEnemy(e, en))
			{
				s->m_hEnemy = EH_NONE;
				en = -1;
			}
		}
		if (Mon_HasConditions(m, bits_COND_LIGHT_DAMAGE | bits_COND_HEAVY_DAMAGE))
			return Mon_GetScheduleOfType(e, SCHED_SMALL_FLINCH);
		if (Mon_HasConditions(m, bits_COND_HEAR_SOUND))
		{
			snd = Mon_PBestSound(e);
			if (snd && (snd->m_iType & (bits_SOUND_DANGER | bits_SOUND_COMBAT)) &&
				gtime() - s->m_fearTime > 3)
			{
				s->m_fearTime = gtime();
				return Mon_GetScheduleOfType(e, SCHED_STARTLE);
			}
		}
		if (Tlk_IsFollowing(e))
		{
			tg = EHGet(s->m_hTargetEnt);
			if (!IsAlive(tg))
			{
				Mon_StopFollowing(e, 0);
				break;
			}
			rel = en >= 0 ? Mon_IRelationshipOf(e, en) : R_NO;
			if (rel != R_DL && rel != R_HT)
			{
				if (Tlk_TargetDistance(e) <= 128)
				{
					if (CanHeal(e))
						return slHeal;
					if (Mon_HasConditions(m, bits_COND_CLIENT_PUSH))
						return Mon_GetScheduleOfType(e, SCHED_MOVE_AWAY_FOLLOW);
				}
				return Mon_GetScheduleOfType(e, SCHED_TARGET_FACE);
			}
			if (Mon_HasConditions(m, bits_COND_NEW_ENEMY))
				return Mon_GetScheduleOfType(e, SCHED_FEAR);
			return Mon_GetScheduleOfType(e, SCHED_TARGET_FACE_SCARED);
		}
		if (Mon_HasConditions(m, bits_COND_CLIENT_PUSH))
			return Mon_GetScheduleOfType(e, SCHED_MOVE_AWAY);
		Tlk_TrySmellTalk(e);
		break;
	case MONSTERSTATE_COMBAT:
		if (Mon_HasConditions(m, bits_COND_NEW_ENEMY))
			return slFear;
		if (Mon_HasConditions(m, bits_COND_SEE_ENEMY))
			return slScientistCover;
		if (Mon_HasConditions(m, bits_COND_HEAR_SOUND))
			return slTakeCoverFromBestSound;
		return slScientistCover;
	}
	return CBaseMonster_GetSchedule(e);
}

static int CScientist_GetIdealState(int e)
{
	CScientist	*s = SCI(e);
	CBaseMonster	*m = (CBaseMonster *)s;
	int		en, rel;

	switch (m->m_MonsterState)
	{
	case MONSTERSTATE_ALERT:
	case MONSTERSTATE_IDLE:
		if (Mon_HasConditions(m, bits_COND_NEW_ENEMY))
		{
			if (Tlk_IsFollowing(e))
			{
				en = EHGet(s->m_hEnemy);
				rel = en >= 0 ? Mon_IRelationshipOf(e, en) : R_NO;
				/* HLSDK's condition, operator precedence included */
				if (rel != R_FR || (rel != R_HT &&
					!Mon_HasConditions(m, bits_COND_LIGHT_DAMAGE | bits_COND_HEAVY_DAMAGE)))
				{
					m->m_IdealMonsterState = MONSTERSTATE_ALERT;
					return m->m_IdealMonsterState;
				}
				Mon_StopFollowing(e, 1);
			}
		}
		else if (Mon_HasConditions(m, bits_COND_LIGHT_DAMAGE | bits_COND_HEAVY_DAMAGE))
		{
			if (Tlk_IsFollowing(e))
				Mon_StopFollowing(e, 1);
		}
		break;
	case MONSTERSTATE_COMBAT:
		en = EHGet(s->m_hEnemy);
		if (en >= 0)
		{
			if (DisregardEnemy(e, en))
			{
				m->m_IdealMonsterState = MONSTERSTATE_ALERT;
				s->m_hEnemy = EH_NONE;
				return m->m_IdealMonsterState;
			}
			if (EHGet(s->m_hTargetEnt) >= 0)
			{
				m->m_IdealMonsterState = MONSTERSTATE_ALERT;
				return m->m_IdealMonsterState;
			}
			if (Mon_HasConditions(m, bits_COND_SEE_ENEMY))
			{
				s->m_fearTime = gtime();
				m->m_IdealMonsterState = MONSTERSTATE_COMBAT;
				return m->m_IdealMonsterState;
			}
		}
		break;
	}
	return CBaseMonster_GetIdealState(e);
}

static int CScientist_FriendNumber(int e, int n)
{
	static const int array[3] = { 1, 2, 0 };

	(void)e;
	return n < 3 ? array[n] : n;
}

static float CScientist_CoverRadius(int e) { (void)e; return 1200; }

static int CScientist_ObjectCaps(int e) { (void)e; return FCAP_ACROSS_TRANSITION | FCAP_IMPULSE_USE; }

static void sci_precache(void)
{
	PRECACHE(RESID(MON_SCIENTIST_MDL));
	PRECACHE(RESID(SCI_PAIN1));
	PRECACHE(RESID(SCI_PAIN2));
	PRECACHE(RESID(SCI_PAIN3));
	PRECACHE(RESID(SCI_PAIN4));
	PRECACHE(RESID(SCI_PAIN5));
}

static int head_key(int e, gid_t16 gid)
{
	/* pev->body == -1 chooses a random head (body is a u8 component) */
	if (KeyF(gid, K_BODY, 0) < 0)
		ent_rnd(e)->body = (uint8_t)RANDOM_LONG(0, NUM_SCIENTIST_HEADS - 1);
	return ent_rnd(e)->body;
}

static int CScientist_Spawn(int e, gid_t16 gid)
{
	CScientist	*s;
	CBaseMonster	*m = Mon_SpawnKeys(e, gid);

	s = (CScientist *)m;
	Tlk_KeyValue(e, gid);
	sci_precache();
	CScientist_TalkInit(e);
	Tlk_Precache(e);
	ent_set_model(e, RESID(MON_SCIENTIST_MDL));
	ent_set_size(e, VEC_HUMAN_HULL_MIN, VEC_HUMAN_HULL_MAX);
	ent_set_solid(e, SOLID_SLIDEBOX);
	ent_set_movetype(e, MOVETYPE_STEP);
	m->m_bloodColor = BLOOD_COLOR_RED;
	m->health = mon_skill(sk_scientistHealth);
	VecSet(m->view_ofs, 0, 0, 50);
	m->m_flFieldOfView = VIEW_FIELD_WIDE;
	m->m_MonsterState = MONSTERSTATE_NONE;
	m->m_afCapability = bits_CAP_HEAR | bits_CAP_TURN_HEAD | bits_CAP_OPEN_DOORS |
		bits_CAP_AUTO_DOORS | bits_CAP_USE;
	ent_cv(e)->skin = 0;
	if (head_key(e, gid) == HEAD_LUTHER)
		ent_cv(e)->skin = 1;
	Mon_MonsterInit(e);
	SetUse(e, FN_CTalkMonster_FollowerUse);
	SetTouch(e, FN_CTalkMonster_Touch);	/* CTalkMonster::Touch override */
	(void)s;
	return 0;
}

static const game_vt_t sci_vt = {
	MON_GAME_VT_BASE,
	.TraceAttack = CBaseMonster_TraceAttack,
	.ObjectCaps = CScientist_ObjectCaps,
	.Classify = CScientist_Classify,
	.TakeDamage = CScientist_TakeDamage,
	.Killed = CScientist_Killed,
	.IRelationship = CTalkMonster_IRelationship,
};

static const mon_vt_t sci_mvt = {
	.SetYawSpeed = CScientist_SetYawSpeed,
	.HandleAnimEvent = CScientist_HandleAnimEvent,
	.RunTask = CScientist_RunTask,
	.StartTask = CScientist_StartTask,
	.SetActivity = CScientist_SetActivity,
	.GetStoppedActivity = CScientist_GetStoppedActivity,
	.ISoundMask = CScientist_ISoundMask,
	.DeclineFollowing = CScientist_DeclineFollowing,
	.CoverRadius = CScientist_CoverRadius,
	.GetScheduleOfType = CScientist_GetScheduleOfType,
	.GetSchedule = CScientist_GetSchedule,
	.GetIdealState = CScientist_GetIdealState,
	.DeathSound = CScientist_PainSound,
	.PainSound = CScientist_PainSound,
	.FriendNumber = CScientist_FriendNumber,
	.TalkInit = CScientist_TalkInit,
	/* CTalkMonster */
	.CanPlaySentence = CTalkMonster_CanPlaySentence,
	.PlaySentence = CTalkMonster_PlaySentence,
	.PlayScriptedSentence = CTalkMonster_PlayScriptedSentence,
	.PrescheduleThink = CTalkMonster_PrescheduleThink,
	.StopFollowing = CTalkMonster_StopFollowing,
	.SetAnswerQuestion = CTalkMonster_SetAnswerQuestion,
	.talk = 1,
};

/* HLSDK CScientist::m_SaveData */
static const sv_field_t f_CScientist[] = {
	SVF(CScientist, m_painTime, SV_TIME, 0),
	SVF(CScientist, m_healTime, SV_TIME, 0),
	SVF(CScientist, m_fearTime, SV_TIME, 0),
};
static const sv_table_t sv_CScientist = SV_TABLE(&sv_CTalkMonster, f_CScientist);

const game_class_t gc_MONSTER_SCIENTIST = {
	.size = sizeof(CScientist),
	.Spawn = CScientist_Spawn,
	.vt = &sci_vt,
	.ext = &sci_mvt,
	.save = &sv_CScientist,
};

/* ---- monster_scientist_dead (CDeadScientist) ------------------------------------------- */

static const char *const sci_poses[] = {
	"lying_on_back", "lying_on_stomach", "dead_sitting", "dead_hang", "dead_table1",
	"dead_table2", "dead_table3"
};

static int CDead_Classify_HumanPassive(int e) { (void)e; return CLASS_HUMAN_PASSIVE; }

static int CDeadScientist_Spawn(int e, gid_t16 gid)
{
	CBaseMonster	*m = Mon_SpawnKeys(e, gid);
	int		pose = (int)KeyF(gid, K_POSE, 0), seq;

	PRECACHE(RESID(MON_SCIENTIST_MDL));
	ent_set_model(e, RESID(MON_SCIENTIST_MDL));
	ent_rnd(e)->effects = 0;
	ent_rnd(e)->sequence = 0;
	m->health = 8;
	m->m_bloodColor = BLOOD_COLOR_RED;
	ent_cv(e)->skin = head_key(e, gid) == HEAD_LUTHER ? 1 : 0;
	if (pose < 0 || pose >= (int)(sizeof(sci_poses) / sizeof(sci_poses[0])))
		pose = 0;
	seq = Mon_LookupSequence(e, sci_poses[pose]);
	if (seq < 0)
	{
		log_printf("Dead scientist with bad pose\n");
		seq = 0;
	}
	ent_rnd(e)->sequence = (uint16_t)seq;
	Mon_MonsterInitDead(e);
	return 0;
}

static const game_vt_t deadsci_vt = {
	MON_GAME_VT,
	.Classify = CDead_Classify_HumanPassive,
};

static const mon_vt_t dead_mvt = { .squad = 0 };

const game_class_t gc_MONSTER_SCIENTIST_DEAD = {
	.size = sizeof(CBaseMonster),
	.Spawn = CDeadScientist_Spawn,
	.vt = &deadsci_vt,
	.ext = &dead_mvt,
	.save = &sv_CBaseMonster,
};
