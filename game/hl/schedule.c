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
 * schedule.c -- the monsters' AI scheduling system (HLSDK dlls/schedule.cpp):
 * schedule bookkeeping, MaintainSchedule, and the shared tasks' StartTask /
 * RunTask.
 */
#include <math.h>

#include "game/hl/monster.h"

int Mon_FHaveSchedule(int e) { return MON(e)->m_pSchedule != 0; }

void Mon_ClearSchedule(int e)
{
	CBaseMonster *m = MON(e);

	m->m_iTaskStatus = TASKSTATUS_NEW;
	m->m_pSchedule = 0;
	m->m_iScheduleIndex = 0;
}

int Mon_FScheduleDone(int e)
{
	CBaseMonster *m = MON(e);

	return m->m_iScheduleIndex == m->m_pSchedule->cTasks;
}

void Mon_ChangeSchedule(int e, const Schedule_t *s)
{
	CBaseMonster *m = MON(e);

	m->m_pSchedule = s;
	m->m_iScheduleIndex = 0;
	m->m_iTaskStatus = TASKSTATUS_NEW;
	m->m_afConditions = 0;
	m->m_failSchedule = SCHED_NONE;
}

void Mon_NextScheduledTask(int e)
{
	CBaseMonster *m = MON(e);

	m->m_iTaskStatus = TASKSTATUS_NEW;
	m->m_iScheduleIndex++;
	if (Mon_FScheduleDone(e))
		Mon_SetConditions(m, bits_COND_SCHEDULE_DONE);
}

int Mon_IScheduleFlags(int e)
{
	CBaseMonster *m = MON(e);

	if (!m->m_pSchedule)
		return 0;
	return m->m_afConditions & m->m_pSchedule->iInterruptMask;
}

int Mon_FScheduleValid(int e)
{
	CBaseMonster *m = MON(e);

	if (!m->m_pSchedule)
		return 0;
	if (Mon_HasConditions(m, m->m_pSchedule->iInterruptMask | bits_COND_SCHEDULE_DONE |
		bits_COND_TASK_FAILED))
		return 0;
	return 1;
}

const Task_t *Mon_GetTask(int e)
{
	CBaseMonster *m = MON(e);

	if (!m->m_pSchedule || m->m_iScheduleIndex < 0 || m->m_iScheduleIndex >= m->m_pSchedule->cTasks)
		return 0;
	return &m->m_pSchedule->pTasklist[m->m_iScheduleIndex];
}

void Mon_MaintainSchedule(int e)
{
	CBaseMonster		*m = MON(e);
	const Schedule_t	*ns;
	const Task_t		*t;
	int			i;

	for (i = 0; i < 10; i++)
	{
		if (m->m_pSchedule && Mon_TaskIsComplete(m))
			Mon_NextScheduledTask(e);
		if (!Mon_FScheduleValid(e) || m->m_MonsterState != m->m_IdealMonsterState)
		{
			Mon_ScheduleChange(e);
			if (m->m_IdealMonsterState != MONSTERSTATE_DEAD &&
				(m->m_IdealMonsterState != MONSTERSTATE_SCRIPT ||
				 m->m_IdealMonsterState == m->m_MonsterState))
			{
				if ((m->m_afConditions && !Mon_HasConditions(m, bits_COND_SCHEDULE_DONE)) ||
					(m->m_pSchedule && (m->m_pSchedule->iInterruptMask & bits_COND_SCHEDULE_DONE)) ||
					(m->m_MonsterState == MONSTERSTATE_COMBAT && EHGet(m->m_hEnemy) < 0))
					Mon_GetIdealState(e);
			}
			if (Mon_HasConditions(m, bits_COND_TASK_FAILED) &&
				m->m_MonsterState == m->m_IdealMonsterState)
			{
				if (m->m_failSchedule != SCHED_NONE)
					ns = Mon_GetScheduleOfType(e, m->m_failSchedule);
				else
					ns = Mon_GetScheduleOfType(e, SCHED_FAIL);
				Mon_ChangeSchedule(e, ns);
			}
			else
			{
				Mon_SetState(e, m->m_IdealMonsterState);
				if (m->m_MonsterState == MONSTERSTATE_SCRIPT || m->m_MonsterState == MONSTERSTATE_DEAD)
					ns = CBaseMonster_GetSchedule(e);
				else
					ns = Mon_GetSchedule(e);
				Mon_ChangeSchedule(e, ns);
			}
		}
		if (m->m_iTaskStatus == TASKSTATUS_NEW)
		{
			t = Mon_GetTask(e);
			if (!t)
			{
				/* an empty schedule: the HLSDK asserts here */
				Mon_ChangeSchedule(e, Mon_GetScheduleOfType(e, SCHED_FAIL));
				t = Mon_GetTask(e);
			}
			Mon_TaskBegin(m);
			Mon_StartTask(e, t);
			if (!ent_is_live(e))
				return;
		}
		if (m->m_Activity != m->m_IdealActivity)
			Mon_SetActivity(e, m->m_IdealActivity);
		if (!Mon_TaskIsComplete(m) && m->m_iTaskStatus != TASKSTATUS_NEW)
			break;
	}
	if (Mon_TaskIsRunning(e))
	{
		t = Mon_GetTask(e);
		if (t)
			Mon_RunTask(e, t);
		if (!ent_is_live(e))
			return;
	}
	if (m->m_Activity != m->m_IdealActivity)
		Mon_SetActivity(e, m->m_IdealActivity);
}

void CBaseMonster_RunTask(int e, const Task_t *t)
{
	CBaseMonster	*m = MON(e);
	col_ent_t	*cv = ent_cv(e);
	int		target, cine;
	float		d[3], dist, mn[3], mx[3];

	switch (t->iTask)
	{
	case TASK_TURN_RIGHT:
	case TASK_TURN_LEFT:
		Mon_ChangeYaw(e, (int)ent_phys(e)->yaw_speed);
		if (Mon_FacingIdeal(e))
			Mon_TaskComplete(m);
		break;
	case TASK_PLAY_SEQUENCE_FACE_ENEMY:
	case TASK_PLAY_SEQUENCE_FACE_TARGET:
		target = EHGet(t->iTask == TASK_PLAY_SEQUENCE_FACE_TARGET ? m->m_hTargetEnt : m->m_hEnemy);
		if (target >= 0)
		{
			VecSub(ent_cv(target)->origin, cv->origin, d);
			ent_phys(e)->ideal_yaw = UTIL_VecToYaw(d);
			Mon_ChangeYaw(e, (int)ent_phys(e)->yaw_speed);
		}
		if (m->anim.m_fSequenceFinished)
			Mon_TaskComplete(m);
		break;
	case TASK_PLAY_SEQUENCE:
	case TASK_PLAY_ACTIVE_IDLE:
		if (m->anim.m_fSequenceFinished)
			Mon_TaskComplete(m);
		break;
	case TASK_FACE_ENEMY:
		Mon_MakeIdealYaw(e, m->m_vecEnemyLKP);
		Mon_ChangeYaw(e, (int)ent_phys(e)->yaw_speed);
		if (Mon_FacingIdeal(e))
			Mon_TaskComplete(m);
		break;
	case TASK_FACE_HINTNODE:
	case TASK_FACE_LASTPOSITION:
	case TASK_FACE_TARGET:
	case TASK_FACE_IDEAL:
	case TASK_FACE_ROUTE:
		Mon_ChangeYaw(e, (int)ent_phys(e)->yaw_speed);
		if (Mon_FacingIdeal(e))
			Mon_TaskComplete(m);
		break;
	case TASK_WAIT_PVS:
		if (ent_client_in_pvs(e) > 0)
			Mon_TaskComplete(m);
		break;
	case TASK_WAIT_INDEFINITE:
		break;
	case TASK_WAIT:
	case TASK_WAIT_RANDOM:
		if (gtime() >= m->m_flWaitFinished)
			Mon_TaskComplete(m);
		break;
	case TASK_WAIT_FACE_ENEMY:
		Mon_MakeIdealYaw(e, m->m_vecEnemyLKP);
		Mon_ChangeYaw(e, (int)ent_phys(e)->yaw_speed);
		if (gtime() >= m->m_flWaitFinished)
			Mon_TaskComplete(m);
		break;
	case TASK_MOVE_TO_TARGET_RANGE:
		target = EHGet(m->m_hTargetEnt);
		if (target < 0)
			Mon_TaskFail(m);
		else
		{
			dist = mv_dist2d(m->m_vecMoveGoal, cv->origin);
			if (dist < t->flData || mv_dist(m->m_vecMoveGoal, ent_cv(target)->origin) > t->flData * 0.5f)
			{
				VecCopy(ent_cv(target)->origin, m->m_vecMoveGoal);
				dist = mv_dist2d(m->m_vecMoveGoal, cv->origin);
				Mon_FRefreshRoute(e);
			}
			if (dist < t->flData)
			{
				Mon_TaskComplete(m);
				Mon_RouteClear(e);
			}
			else if (dist < 190 && m->m_movementActivity != ACT_WALK)
				m->m_movementActivity = ACT_WALK;
			else if (dist >= 270 && m->m_movementActivity != ACT_RUN)
				m->m_movementActivity = ACT_RUN;
		}
		break;
	case TASK_WAIT_FOR_MOVEMENT:
		if (Mon_MovementIsComplete(m))
		{
			Mon_TaskComplete(m);
			Mon_RouteClear(e);
		}
		break;
	case TASK_DIE:
		if (m->anim.m_fSequenceFinished && ent_rnd(e)->frame >= 255)
		{
			m->deadflag = DEAD_DEAD;
			SetThink(e, FN_NONE);
			ent_think_at(e, 0);
			ent_rnd(e)->framerate = 0;	/* StopAnimation */
			if (!Mon_BBoxFlat(e))
			{
				VecSet(mn, -4, -4, 0);
				VecSet(mx, 4, 4, 1);
			}
			else
			{
				VecCopy(cv->mins, mn);
				VecSet(mx, cv->maxs[0], cv->maxs[1], cv->mins[2] + 1);
			}
			ent_set_size(e, mn, mx);
			if (Mon_ShouldFadeOnDeath(e))
				SUB_StartFadeOut(e);
			else
				CSoundEnt_InsertSound(bits_SOUND_CARCASS, cv->origin, 384, 30);
		}
		break;
	case TASK_RANGE_ATTACK1_NOTURN:
	case TASK_MELEE_ATTACK1_NOTURN:
	case TASK_MELEE_ATTACK2_NOTURN:
	case TASK_RANGE_ATTACK2_NOTURN:
	case TASK_RELOAD_NOTURN:
		if (m->anim.m_fSequenceFinished)
		{
			m->m_Activity = ACT_RESET;
			Mon_TaskComplete(m);
		}
		break;
	case TASK_RANGE_ATTACK1:
	case TASK_MELEE_ATTACK1:
	case TASK_MELEE_ATTACK2:
	case TASK_RANGE_ATTACK2:
	case TASK_SPECIAL_ATTACK1:
	case TASK_SPECIAL_ATTACK2:
	case TASK_RELOAD:
		Mon_MakeIdealYaw(e, m->m_vecEnemyLKP);
		Mon_ChangeYaw(e, (int)ent_phys(e)->yaw_speed);
		if (m->anim.m_fSequenceFinished)
		{
			m->m_Activity = ACT_RESET;
			Mon_TaskComplete(m);
		}
		break;
	case TASK_SMALL_FLINCH:
		if (m->anim.m_fSequenceFinished)
			Mon_TaskComplete(m);
		break;
	case TASK_WAIT_FOR_SCRIPT:
		cine = EHGet(m->m_pCine);
		if (cine < 0)
		{
			Mon_TaskFail(m);
			break;
		}
		if (Script_ReadyToStart(cine))
		{
			Mon_TaskComplete(m);
			Script_StartSequence(cine, e, 1, 1);
			if (m->anim.m_fSequenceFinished)
				Mon_ClearSchedule(e);
			ent_rnd(e)->framerate = 1.0f;
		}
		break;
	case TASK_PLAY_SCRIPT:
		cine = EHGet(m->m_pCine);
		if (m->anim.m_fSequenceFinished)
		{
			if (cine >= 0)
				Script_SequenceDone(cine, e);
			else
				Mon_CineCleanup(e);
		}
		break;
	}
}

void Mon_SetTurnActivity(int e)
{
	CBaseMonster	*m = MON(e);
	float		yd = Mon_FlYawDiff(e);

	if (yd <= -45 && Mon_LookupActivity(e, ACT_TURN_RIGHT) != ACTIVITY_NOT_AVAILABLE)
		m->m_IdealActivity = ACT_TURN_RIGHT;
	else if (yd > 45 && Mon_LookupActivity(e, ACT_TURN_LEFT) != ACTIVITY_NOT_AVAILABLE)
		m->m_IdealActivity = ACT_TURN_LEFT;
}

static int enemy_or_fail(CBaseMonster *m)
{
	int en = EHGet(m->m_hEnemy);

	if (en < 0)
		Mon_TaskFail(m);
	return en;
}

static const float *view_ofs_of(int e)
{
	static const float zero[3];
	return ent_priv(e) ? CBase(e)->view_ofs : zero;
}

void CBaseMonster_StartTask(int e, const Task_t *t)
{
	CBaseMonster	*m = MON(e);
	col_ent_t	*cv = ent_cv(e);
	int		en, target, cine, act, cover;
	float		cur, d[3], p[3], fwd[3], rt[3];
	CSound		*s;
	const Schedule_t *ns;

	switch (t->iTask)
	{
	case TASK_TURN_RIGHT:
		cur = UTIL_AngleMod(cv->angles[1]);
		ent_phys(e)->ideal_yaw = UTIL_AngleMod(cur - t->flData);
		Mon_SetTurnActivity(e);
		break;
	case TASK_TURN_LEFT:
		cur = UTIL_AngleMod(cv->angles[1]);
		ent_phys(e)->ideal_yaw = UTIL_AngleMod(cur + t->flData);
		Mon_SetTurnActivity(e);
		break;
	case TASK_REMEMBER:
		Mon_Remember(m, (int)t->flData);
		Mon_TaskComplete(m);
		break;
	case TASK_FORGET:
		Mon_Forget(m, (int)t->flData);
		Mon_TaskComplete(m);
		break;
	case TASK_FIND_HINTNODE:
		m->m_iHintNode = Mon_FindHintNode(e);
		if (m->m_iHintNode != NO_NODE)
			Mon_TaskComplete(m);
		else
			Mon_TaskFail(m);
		break;
	case TASK_STORE_LASTPOSITION:
		VecCopy(cv->origin, m->m_vecLastPosition);
		Mon_TaskComplete(m);
		break;
	case TASK_CLEAR_LASTPOSITION:
		VecSet(m->m_vecLastPosition, 0, 0, 0);
		Mon_TaskComplete(m);
		break;
	case TASK_CLEAR_HINTNODE:
		m->m_iHintNode = NO_NODE;
		Mon_TaskComplete(m);
		break;
	case TASK_STOP_MOVING:
		if (m->m_IdealActivity == m->m_movementActivity)
			m->m_IdealActivity = Mon_GetStoppedActivity(e);
		Mon_RouteClear(e);
		Mon_TaskComplete(m);
		break;
	case TASK_PLAY_SEQUENCE_FACE_ENEMY:
	case TASK_PLAY_SEQUENCE_FACE_TARGET:
	case TASK_PLAY_SEQUENCE:
		m->m_IdealActivity = (int)t->flData;
		break;
	case TASK_PLAY_ACTIVE_IDLE:
		/* hint nodes: none in the cooked graph (FindHintNode) */
		m->m_IdealActivity = ACT_IDLE;
		break;
	case TASK_SET_SCHEDULE:
		ns = Mon_GetScheduleOfType(e, (int)t->flData);
		if (ns)
			Mon_ChangeSchedule(e, ns);
		else
			Mon_TaskFail(m);
		break;
	case TASK_FIND_NEAR_NODE_COVER_FROM_ENEMY:
		if ((en = enemy_or_fail(m)) < 0)
			return;
		if (Mon_FindCover(e, ent_cv(en)->origin, view_ofs_of(en), 0, t->flData))
			Mon_TaskComplete(m);
		else
			Mon_TaskFail(m);
		break;
	case TASK_FIND_FAR_NODE_COVER_FROM_ENEMY:
		if ((en = enemy_or_fail(m)) < 0)
			return;
		if (Mon_FindCover(e, ent_cv(en)->origin, view_ofs_of(en), t->flData, Mon_CoverRadius(e)))
			Mon_TaskComplete(m);
		else
			Mon_TaskFail(m);
		break;
	case TASK_FIND_NODE_COVER_FROM_ENEMY:
		if ((en = enemy_or_fail(m)) < 0)
			return;
		if (Mon_FindCover(e, ent_cv(en)->origin, view_ofs_of(en), 0, Mon_CoverRadius(e)))
			Mon_TaskComplete(m);
		else
			Mon_TaskFail(m);
		break;
	case TASK_FIND_COVER_FROM_ENEMY:
		en = EHGet(m->m_hEnemy);
		cover = en >= 0 ? en : e;
		{
			float org[3], vo[3];

			VecCopy(ent_cv(cover)->origin, org);
			VecCopy(view_ofs_of(cover), vo);
			if (Mon_FindLateralCover(e, org, vo) ||
				Mon_FindCover(e, org, vo, 0, Mon_CoverRadius(e)))
			{
				m->m_flMoveWaitFinished = gtime() + t->flData;
				Mon_TaskComplete(m);
			}
			else
				Mon_TaskFail(m);
		}
		break;
	case TASK_FIND_COVER_FROM_ORIGIN:
		{
			float org[3];

			VecCopy(cv->origin, org);
			if (Mon_FindCover(e, org, m->view_ofs, 0, Mon_CoverRadius(e)))
			{
				m->m_flMoveWaitFinished = gtime() + t->flData;
				Mon_TaskComplete(m);
			}
			else
				Mon_TaskFail(m);
		}
		break;
	case TASK_FIND_COVER_FROM_BEST_SOUND:
		s = Mon_PBestSound(e);
		if (s && Mon_FindCover(e, s->m_vecOrigin, g_vecZero, (float)s->m_iVolume, Mon_CoverRadius(e)))
		{
			m->m_flMoveWaitFinished = gtime() + t->flData;
			Mon_TaskComplete(m);
		}
		else
			Mon_TaskFail(m);
		break;
	case TASK_FACE_HINTNODE:
		Mon_SetTurnActivity(e);
		break;
	case TASK_FACE_LASTPOSITION:
		Mon_MakeIdealYaw(e, m->m_vecLastPosition);
		Mon_SetTurnActivity(e);
		break;
	case TASK_FACE_TARGET:
		target = EHGet(m->m_hTargetEnt);
		if (target >= 0)
		{
			Mon_MakeIdealYaw(e, ent_cv(target)->origin);
			Mon_SetTurnActivity(e);
		}
		else
			Mon_TaskFail(m);
		break;
	case TASK_FACE_ENEMY:
		Mon_MakeIdealYaw(e, m->m_vecEnemyLKP);
		Mon_SetTurnActivity(e);
		break;
	case TASK_FACE_IDEAL:
		Mon_SetTurnActivity(e);
		break;
	case TASK_FACE_ROUTE:
		if (Mon_FRouteClear(e))
			Mon_TaskFail(m);
		else
		{
			Mon_MakeIdealYaw(e, m->m_Route[m->m_iRouteIndex].vecLocation);
			Mon_SetTurnActivity(e);
		}
		break;
	case TASK_WAIT_PVS:
	case TASK_WAIT_INDEFINITE:
		break;
	case TASK_WAIT:
	case TASK_WAIT_FACE_ENEMY:
		m->m_flWaitFinished = gtime() + t->flData;
		break;
	case TASK_WAIT_RANDOM:
		m->m_flWaitFinished = gtime() + RANDOM_FLOAT(0.1f, t->flData);
		break;
	case TASK_MOVE_TO_TARGET_RANGE:
		target = EHGet(m->m_hTargetEnt);
		if (target < 0)
		{
			Mon_TaskFail(m);
			break;
		}
		if (mv_dist(ent_cv(target)->origin, cv->origin) < 1)
			Mon_TaskComplete(m);
		else
		{
			VecCopy(ent_cv(target)->origin, m->m_vecMoveGoal);
			if (!Mon_MoveToTarget(e, ACT_WALK, 2))
				Mon_TaskFail(m);
		}
		break;
	case TASK_RUN_TO_TARGET:
	case TASK_WALK_TO_TARGET:
		target = EHGet(m->m_hTargetEnt);
		if (target < 0)
		{
			Mon_TaskFail(m);
			Mon_RouteClear(e);
		}
		else if (mv_dist(ent_cv(target)->origin, cv->origin) < 1)
			Mon_TaskComplete(m);
		else
		{
			act = t->iTask == TASK_WALK_TO_TARGET ? ACT_WALK : ACT_RUN;
			if (Mon_LookupActivity(e, act) == ACTIVITY_NOT_AVAILABLE)
				Mon_TaskComplete(m);
			else if (!Mon_MoveToTarget(e, act, 2))
			{
				Mon_TaskFail(m);
				Mon_RouteClear(e);
			}
		}
		Mon_TaskComplete(m);
		break;
	case TASK_CLEAR_MOVE_WAIT:
		m->m_flMoveWaitFinished = gtime();
		Mon_TaskComplete(m);
		break;
	case TASK_MELEE_ATTACK1_NOTURN:
	case TASK_MELEE_ATTACK1:
		m->m_IdealActivity = ACT_MELEE_ATTACK1;
		break;
	case TASK_MELEE_ATTACK2_NOTURN:
	case TASK_MELEE_ATTACK2:
		m->m_IdealActivity = ACT_MELEE_ATTACK2;
		break;
	case TASK_RANGE_ATTACK1_NOTURN:
	case TASK_RANGE_ATTACK1:
		m->m_IdealActivity = ACT_RANGE_ATTACK1;
		break;
	case TASK_RANGE_ATTACK2_NOTURN:
	case TASK_RANGE_ATTACK2:
		m->m_IdealActivity = ACT_RANGE_ATTACK2;
		break;
	case TASK_RELOAD_NOTURN:
	case TASK_RELOAD:
		m->m_IdealActivity = ACT_RELOAD;
		break;
	case TASK_SPECIAL_ATTACK1:
		m->m_IdealActivity = ACT_SPECIAL_ATTACK1;
		break;
	case TASK_SPECIAL_ATTACK2:
		m->m_IdealActivity = ACT_SPECIAL_ATTACK2;
		break;
	case TASK_SET_ACTIVITY:
		m->m_IdealActivity = (int)t->flData;
		Mon_TaskComplete(m);
		break;
	case TASK_GET_PATH_TO_ENEMY_LKP:
		VecCopy(m->m_vecEnemyLKP, p);
		if (Mon_BuildRoute(e, p, bits_MF_TO_LOCATION, -1))
			Mon_TaskComplete(m);
		else if (Mon_BuildNearestRoute(e, p, m->view_ofs, 0, mv_dist(p, cv->origin)))
			Mon_TaskComplete(m);
		else
			Mon_TaskFail(m);
		break;
	case TASK_GET_PATH_TO_ENEMY:
		if ((en = enemy_or_fail(m)) < 0)
			return;
		VecCopy(ent_cv(en)->origin, p);
		if (Mon_BuildRoute(e, p, bits_MF_TO_ENEMY, en))
			Mon_TaskComplete(m);
		else if (Mon_BuildNearestRoute(e, p, view_ofs_of(en), 0, mv_dist(p, cv->origin)))
			Mon_TaskComplete(m);
		else
			Mon_TaskFail(m);
		break;
	case TASK_GET_PATH_TO_ENEMY_CORPSE:
		UTIL_MakeVectors(cv->angles);
		VecMA(m->m_vecEnemyLKP, -64, gpGlobals.v_forward, p);
		if (Mon_BuildRoute(e, p, bits_MF_TO_LOCATION, -1))
			Mon_TaskComplete(m);
		else
			Mon_TaskFail(m);
		break;
	case TASK_GET_PATH_TO_SPOT:
		VecCopy(m->m_vecMoveGoal, p);
		if (Mon_BuildRoute(e, p, bits_MF_TO_LOCATION, ENT_PLAYER))
			Mon_TaskComplete(m);
		else
			Mon_TaskFail(m);
		break;
	case TASK_GET_PATH_TO_TARGET:
		Mon_RouteClear(e);
		if (EHGet(m->m_hTargetEnt) >= 0 && Mon_MoveToTarget(e, m->m_movementActivity, 1))
			Mon_TaskComplete(m);
		else
			Mon_TaskFail(m);
		break;
	case TASK_GET_PATH_TO_HINTNODE:
		Mon_TaskFail(m);	/* no hint nodes */
		break;
	case TASK_GET_PATH_TO_LASTPOSITION:
		VecCopy(m->m_vecLastPosition, m->m_vecMoveGoal);
		VecCopy(m->m_vecMoveGoal, p);
		if (Mon_MoveToLocation(e, m->m_movementActivity, 2, p))
			Mon_TaskComplete(m);
		else
			Mon_TaskFail(m);
		break;
	case TASK_GET_PATH_TO_BESTSOUND:
		s = Mon_PBestSound(e);
		if (s && Mon_MoveToLocation(e, m->m_movementActivity, 2, s->m_vecOrigin))
			Mon_TaskComplete(m);
		else
			Mon_TaskFail(m);
		break;
	case TASK_GET_PATH_TO_BESTSCENT:
		s = Mon_PBestScent(e);
		if (s && Mon_MoveToLocation(e, m->m_movementActivity, 2, s->m_vecOrigin))
			Mon_TaskComplete(m);
		else
			Mon_TaskFail(m);
		break;
	case TASK_RUN_PATH:
		m->m_movementActivity = Mon_LookupActivity(e, ACT_RUN) != ACTIVITY_NOT_AVAILABLE ?
			ACT_RUN : ACT_WALK;
		Mon_TaskComplete(m);
		break;
	case TASK_WALK_PATH:
		if (cv->movetype == MOVETYPE_FLY)
			m->m_movementActivity = ACT_FLY;
		m->m_movementActivity = Mon_LookupActivity(e, ACT_WALK) != ACTIVITY_NOT_AVAILABLE ?
			ACT_WALK : ACT_RUN;
		Mon_TaskComplete(m);
		break;
	case TASK_STRAFE_PATH:
		UTIL_MakeVectors(cv->angles);
		VecSub(m->m_Route[0].vecLocation, cv->origin, d);
		mv_norm2d(fwd, d);
		mv_norm2d(rt, gpGlobals.v_right);
		m->m_movementActivity = fwd[0] * rt[0] + fwd[1] * rt[1] > 0 ? ACT_STRAFE_RIGHT :
			ACT_STRAFE_LEFT;
		Mon_TaskComplete(m);
		break;
	case TASK_WAIT_FOR_MOVEMENT:
		if (Mon_FRouteClear(e))
			Mon_TaskComplete(m);
		break;
	case TASK_EAT:
		Mon_Eat(e, t->flData);
		Mon_TaskComplete(m);
		break;
	case TASK_SMALL_FLINCH:
		m->m_IdealActivity = Mon_GetSmallFlinchActivity(e);
		break;
	case TASK_DIE:
		Mon_RouteClear(e);
		m->m_IdealActivity = Mon_GetDeathActivity(e);
		m->deadflag = DEAD_DYING;
		break;
	case TASK_SOUND_WAKE:
		Mon_AlertSound(e);
		Mon_TaskComplete(m);
		break;
	case TASK_SOUND_DIE:
	case TASK_SOUND_DEATH:
		Mon_DeathSound(e);
		Mon_TaskComplete(m);
		break;
	case TASK_SOUND_IDLE:
		Mon_IdleSound(e);
		Mon_TaskComplete(m);
		break;
	case TASK_SOUND_PAIN:
		Mon_PainSound(e);
		Mon_TaskComplete(m);
		break;
	case TASK_SOUND_ANGRY:
		Mon_TaskComplete(m);
		break;
	case TASK_WAIT_FOR_SCRIPT:
		cine = EHGet(m->m_pCine);
		if (cine < 0)
		{
			Mon_TaskFail(m);
			break;
		}
		if (Script_HasIdle(cine))
		{
			Script_StartSequence(cine, e, 0, 0);
			if (Script_IdleIsPlay(cine))
				ent_rnd(e)->framerate = 0;
		}
		else
			m->m_IdealActivity = ACT_IDLE;
		break;
	case TASK_PLAY_SCRIPT:
		ent_set_movetype(e, MOVETYPE_FLY);
		EFlagsClear(e, FL_ONGROUND);
		m->m_scriptState = SCRIPT_PLAYING;
		break;
	case TASK_ENABLE_SCRIPT:
		cine = EHGet(m->m_pCine);
		if (cine >= 0)
			Script_DelayStart(cine, 0);
		Mon_TaskComplete(m);
		break;
	case TASK_PLANT_ON_SCRIPT:
		target = EHGet(m->m_hTargetEnt);
		if (target >= 0)
			ent_set_origin(e, ent_cv(target)->origin);
		Mon_TaskComplete(m);
		break;
	case TASK_FACE_SCRIPT:
		target = EHGet(m->m_hTargetEnt);
		if (target >= 0)
			ent_phys(e)->ideal_yaw = UTIL_AngleMod(ent_cv(target)->angles[1]);
		Mon_TaskComplete(m);
		m->m_IdealActivity = ACT_IDLE;
		Mon_RouteClear(e);
		break;
	case TASK_SUGGEST_STATE:
		m->m_IdealMonsterState = (int)t->flData;
		Mon_TaskComplete(m);
		break;
	case TASK_SET_FAIL_SCHEDULE:
		m->m_failSchedule = (int)t->flData;
		Mon_TaskComplete(m);
		break;
	case TASK_CLEAR_FAIL_SCHEDULE:
		m->m_failSchedule = SCHED_NONE;
		Mon_TaskComplete(m);
		break;
	default:
		break;
	}
}

const Schedule_t *CBaseMonster_GetSchedule(int e)
{
	CBaseMonster *m = MON(e);

	switch (m->m_MonsterState)
	{
	case MONSTERSTATE_PRONE:
		return Mon_GetScheduleOfType(e, SCHED_BARNACLE_VICTIM_GRAB);
	case MONSTERSTATE_NONE:
		break;
	case MONSTERSTATE_IDLE:
		if (Mon_HasConditions(m, bits_COND_HEAR_SOUND))
			return Mon_GetScheduleOfType(e, SCHED_ALERT_FACE);
		if (Mon_FRouteClear(e))
			return Mon_GetScheduleOfType(e, SCHED_IDLE_STAND);
		return Mon_GetScheduleOfType(e, SCHED_IDLE_WALK);
	case MONSTERSTATE_ALERT:
		if (Mon_HasConditions(m, bits_COND_ENEMY_DEAD) &&
			Mon_LookupActivity(e, ACT_VICTORY_DANCE) != ACTIVITY_NOT_AVAILABLE)
			return Mon_GetScheduleOfType(e, SCHED_VICTORY_DANCE);
		if (Mon_HasConditions(m, bits_COND_LIGHT_DAMAGE | bits_COND_HEAVY_DAMAGE))
		{
			if (fabsf(Mon_FlYawDiff(e)) < (1.0f - m->m_flFieldOfView) * 60)
				return Mon_GetScheduleOfType(e, SCHED_TAKE_COVER_FROM_ORIGIN);
			return Mon_GetScheduleOfType(e, SCHED_ALERT_SMALL_FLINCH);
		}
		if (Mon_HasConditions(m, bits_COND_HEAR_SOUND))
			return Mon_GetScheduleOfType(e, SCHED_ALERT_FACE);
		return Mon_GetScheduleOfType(e, SCHED_ALERT_STAND);
	case MONSTERSTATE_COMBAT:
		if (Mon_HasConditions(m, bits_COND_ENEMY_DEAD))
		{
			m->m_hEnemy = EH_NONE;
			if (Mon_GetEnemy(e))
			{
				Mon_ClearConditions(m, bits_COND_ENEMY_DEAD);
				return Mon_GetSchedule(e);
			}
			Mon_SetState(e, MONSTERSTATE_ALERT);
			return Mon_GetSchedule(e);
		}
		if (Mon_HasConditions(m, bits_COND_NEW_ENEMY))
			return Mon_GetScheduleOfType(e, SCHED_WAKE_ANGRY);
		if (Mon_HasConditions(m, bits_COND_LIGHT_DAMAGE) && !Mon_HasMemory(m, bits_MEMORY_FLINCHED))
			return Mon_GetScheduleOfType(e, SCHED_SMALL_FLINCH);
		if (!Mon_HasConditions(m, bits_COND_SEE_ENEMY))
		{
			if (!Mon_HasConditions(m, bits_COND_ENEMY_OCCLUDED))
				return Mon_GetScheduleOfType(e, SCHED_COMBAT_FACE);
			return Mon_GetScheduleOfType(e, SCHED_CHASE_ENEMY);
		}
		if (Mon_HasConditions(m, bits_COND_CAN_RANGE_ATTACK1))
			return Mon_GetScheduleOfType(e, SCHED_RANGE_ATTACK1);
		if (Mon_HasConditions(m, bits_COND_CAN_RANGE_ATTACK2))
			return Mon_GetScheduleOfType(e, SCHED_RANGE_ATTACK2);
		if (Mon_HasConditions(m, bits_COND_CAN_MELEE_ATTACK1))
			return Mon_GetScheduleOfType(e, SCHED_MELEE_ATTACK1);
		if (Mon_HasConditions(m, bits_COND_CAN_MELEE_ATTACK2))
			return Mon_GetScheduleOfType(e, SCHED_MELEE_ATTACK2);
		if (!Mon_HasConditions(m, bits_COND_CAN_RANGE_ATTACK1 | bits_COND_CAN_MELEE_ATTACK1))
			return Mon_GetScheduleOfType(e, SCHED_CHASE_ENEMY);
		if (!Mon_FacingIdeal(e))
			return Mon_GetScheduleOfType(e, SCHED_COMBAT_FACE);
		break;
	case MONSTERSTATE_DEAD:
		return Mon_GetScheduleOfType(e, SCHED_DIE);
	case MONSTERSTATE_SCRIPT:
		if (EHGet(m->m_pCine) < 0)
		{
			Mon_CineCleanup(e);
			return Mon_GetScheduleOfType(e, SCHED_IDLE_STAND);
		}
		return Mon_GetScheduleOfType(e, SCHED_AISCRIPT);
	default:
		break;
	}
	return Mon_ScheduleError();
}
