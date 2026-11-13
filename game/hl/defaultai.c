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
 * defaultai.c -- the default schedules every monster uses (HLSDK
 * dlls/defaultai.cpp) and CBaseMonster::GetScheduleOfType.
 */
#include "game/hl/monster.h"

#define T(t, d)		{ t, (float)(d) }

#define COND_IDLE_STAND (bits_COND_NEW_ENEMY | bits_COND_SEE_FEAR | bits_COND_LIGHT_DAMAGE | \
	bits_COND_HEAVY_DAMAGE | bits_COND_HEAR_SOUND | bits_COND_SMELL_FOOD | bits_COND_SMELL | \
	bits_COND_PROVOKED)
#define SND_IDLE_STAND (bits_SOUND_COMBAT | bits_SOUND_WORLD | bits_SOUND_PLAYER | bits_SOUND_DANGER | \
	bits_SOUND_MEAT | bits_SOUND_CARCASS | bits_SOUND_GARBAGE)

static const Task_t tlFail[] = {
	T(TASK_STOP_MOVING, 0), T(TASK_SET_ACTIVITY, ACT_IDLE), T(TASK_WAIT, 2), T(TASK_WAIT_PVS, 0),
};
const Schedule_t slFail[] = { SCHED(tlFail, bits_COND_CAN_ATTACK, 0, "Fail") };

static const Task_t tlIdleStand1[] = {
	T(TASK_STOP_MOVING, 0), T(TASK_SET_ACTIVITY, ACT_IDLE), T(TASK_WAIT, 5),
};
const Schedule_t slIdleStand[] = { SCHED(tlIdleStand1, COND_IDLE_STAND, SND_IDLE_STAND, "IdleStand") };
const Schedule_t slIdleTrigger[] = {
	SCHED(tlIdleStand1, bits_COND_LIGHT_DAMAGE | bits_COND_HEAVY_DAMAGE, 0, "Idle Trigger")
};

static const Task_t tlIdleWalk1[] = { T(TASK_WALK_PATH, 9999), T(TASK_WAIT_FOR_MOVEMENT, 0) };
const Schedule_t slIdleWalk[] = {
	SCHED(tlIdleWalk1, bits_COND_NEW_ENEMY | bits_COND_LIGHT_DAMAGE | bits_COND_HEAVY_DAMAGE |
		bits_COND_HEAR_SOUND | bits_COND_SMELL_FOOD | bits_COND_SMELL | bits_COND_PROVOKED,
		bits_SOUND_COMBAT | bits_SOUND_MEAT | bits_SOUND_CARCASS | bits_SOUND_GARBAGE, "Idle Walk")
};

static const Task_t tlAmbush[] = {
	T(TASK_STOP_MOVING, 0), T(TASK_SET_ACTIVITY, ACT_IDLE), T(TASK_WAIT_INDEFINITE, 0),
};
const Schedule_t slAmbush[] = {
	SCHED(tlAmbush, bits_COND_NEW_ENEMY | bits_COND_LIGHT_DAMAGE | bits_COND_HEAVY_DAMAGE |
		bits_COND_PROVOKED, 0, "Ambush")
};

static const Task_t tlActiveIdle[] = {
	T(TASK_FIND_HINTNODE, 0), T(TASK_GET_PATH_TO_HINTNODE, 0), T(TASK_STORE_LASTPOSITION, 0),
	T(TASK_WALK_PATH, 0), T(TASK_WAIT_FOR_MOVEMENT, 0), T(TASK_FACE_HINTNODE, 0),
	T(TASK_PLAY_ACTIVE_IDLE, 0), T(TASK_GET_PATH_TO_LASTPOSITION, 0), T(TASK_WALK_PATH, 0),
	T(TASK_WAIT_FOR_MOVEMENT, 0), T(TASK_CLEAR_LASTPOSITION, 0), T(TASK_CLEAR_HINTNODE, 0),
};
const Schedule_t slActiveIdle[] = {
	SCHED(tlActiveIdle, bits_COND_NEW_ENEMY | bits_COND_LIGHT_DAMAGE | bits_COND_HEAVY_DAMAGE |
		bits_COND_PROVOKED | bits_COND_HEAR_SOUND,
		bits_SOUND_COMBAT | bits_SOUND_WORLD | bits_SOUND_PLAYER | bits_SOUND_DANGER, "Active Idle")
};

static const Task_t tlWakeAngry1[] = {
	T(TASK_STOP_MOVING, 0), T(TASK_SET_ACTIVITY, ACT_IDLE), T(TASK_SOUND_WAKE, 0),
	T(TASK_FACE_IDEAL, 0),
};
const Schedule_t slWakeAngry[] = { SCHED(tlWakeAngry1, 0, 0, "Wake Angry") };

static const Task_t tlAlertFace1[] = {
	T(TASK_STOP_MOVING, 0), T(TASK_SET_ACTIVITY, ACT_IDLE), T(TASK_FACE_IDEAL, 0),
};
const Schedule_t slAlertFace[] = {
	SCHED(tlAlertFace1, bits_COND_NEW_ENEMY | bits_COND_SEE_FEAR | bits_COND_LIGHT_DAMAGE |
		bits_COND_HEAVY_DAMAGE | bits_COND_PROVOKED, 0, "Alert Face")
};

static const Task_t tlAlertSmallFlinch[] = {
	T(TASK_STOP_MOVING, 0), T(TASK_REMEMBER, bits_MEMORY_FLINCHED), T(TASK_SMALL_FLINCH, 0),
	T(TASK_SET_SCHEDULE, SCHED_ALERT_FACE),
};
const Schedule_t slAlertSmallFlinch[] = { SCHED(tlAlertSmallFlinch, 0, 0, "Alert Small Flinch") };

static const Task_t tlAlertStand1[] = {
	T(TASK_STOP_MOVING, 0), T(TASK_SET_ACTIVITY, ACT_IDLE), T(TASK_WAIT, 20),
	T(TASK_SUGGEST_STATE, MONSTERSTATE_IDLE),
};
const Schedule_t slAlertStand[] = {
	SCHED(tlAlertStand1, bits_COND_NEW_ENEMY | bits_COND_SEE_ENEMY | bits_COND_SEE_FEAR |
		bits_COND_LIGHT_DAMAGE | bits_COND_HEAVY_DAMAGE | bits_COND_PROVOKED | bits_COND_SMELL |
		bits_COND_SMELL_FOOD | bits_COND_HEAR_SOUND, SND_IDLE_STAND, "Alert Stand")
};

static const Task_t tlInvestigateSound[] = {
	T(TASK_STOP_MOVING, 0), T(TASK_STORE_LASTPOSITION, 0), T(TASK_GET_PATH_TO_BESTSOUND, 0),
	T(TASK_FACE_IDEAL, 0), T(TASK_WALK_PATH, 0), T(TASK_WAIT_FOR_MOVEMENT, 0),
	T(TASK_PLAY_SEQUENCE, ACT_IDLE), T(TASK_WAIT, 10), T(TASK_GET_PATH_TO_LASTPOSITION, 0),
	T(TASK_WALK_PATH, 0), T(TASK_WAIT_FOR_MOVEMENT, 0), T(TASK_CLEAR_LASTPOSITION, 0),
};
const Schedule_t slInvestigateSound[] = {
	SCHED(tlInvestigateSound, bits_COND_NEW_ENEMY | bits_COND_SEE_FEAR | bits_COND_LIGHT_DAMAGE |
		bits_COND_HEAVY_DAMAGE | bits_COND_HEAR_SOUND, bits_SOUND_DANGER, "InvestigateSound")
};

static const Task_t tlCombatStand1[] = {
	T(TASK_STOP_MOVING, 0), T(TASK_SET_ACTIVITY, ACT_IDLE), T(TASK_WAIT_INDEFINITE, 0),
};
const Schedule_t slCombatStand[] = {
	SCHED(tlCombatStand1, bits_COND_NEW_ENEMY | bits_COND_ENEMY_DEAD | bits_COND_LIGHT_DAMAGE |
		bits_COND_HEAVY_DAMAGE | bits_COND_CAN_ATTACK, 0, "Combat Stand")
};

static const Task_t tlCombatFace1[] = {
	T(TASK_STOP_MOVING, 0), T(TASK_SET_ACTIVITY, ACT_IDLE), T(TASK_FACE_ENEMY, 0),
};
const Schedule_t slCombatFace[] = {
	SCHED(tlCombatFace1, bits_COND_CAN_ATTACK | bits_COND_NEW_ENEMY | bits_COND_ENEMY_DEAD, 0,
		"Combat Face")
};

static const Task_t tlStandoff[] = {
	T(TASK_STOP_MOVING, 0), T(TASK_SET_ACTIVITY, ACT_IDLE), T(TASK_WAIT_FACE_ENEMY, 2),
};
const Schedule_t slStandoff[] = {
	SCHED(tlStandoff, bits_COND_CAN_RANGE_ATTACK1 | bits_COND_CAN_RANGE_ATTACK2 |
		bits_COND_ENEMY_DEAD | bits_COND_NEW_ENEMY | bits_COND_HEAR_SOUND, bits_SOUND_DANGER,
		"Standoff")
};

static const Task_t tlArmWeapon[] = { T(TASK_STOP_MOVING, 0), T(TASK_PLAY_SEQUENCE, ACT_ARM) };
const Schedule_t slArmWeapon[] = { SCHED(tlArmWeapon, 0, 0, "Arm Weapon") };

static const Task_t tlReload[] = { T(TASK_STOP_MOVING, 0), T(TASK_PLAY_SEQUENCE, ACT_RELOAD) };
const Schedule_t slReload[] = { SCHED(tlReload, bits_COND_HEAVY_DAMAGE, 0, "Reload") };

#define COND_ATTACK (bits_COND_NEW_ENEMY | bits_COND_ENEMY_DEAD | bits_COND_LIGHT_DAMAGE | \
	bits_COND_HEAVY_DAMAGE | bits_COND_ENEMY_OCCLUDED)

static const Task_t tlRangeAttack1[] = {
	T(TASK_STOP_MOVING, 0), T(TASK_FACE_ENEMY, 0), T(TASK_RANGE_ATTACK1, 0),
};
const Schedule_t slRangeAttack1[] = {
	SCHED(tlRangeAttack1, COND_ATTACK | bits_COND_NO_AMMO_LOADED | bits_COND_HEAR_SOUND,
		bits_SOUND_DANGER, "Range Attack1")
};

static const Task_t tlRangeAttack2[] = {
	T(TASK_STOP_MOVING, 0), T(TASK_FACE_ENEMY, 0), T(TASK_RANGE_ATTACK2, 0),
};
const Schedule_t slRangeAttack2[] = {
	SCHED(tlRangeAttack2, COND_ATTACK | bits_COND_HEAR_SOUND, bits_SOUND_DANGER, "Range Attack2")
};

static const Task_t tlPrimaryMeleeAttack1[] = {
	T(TASK_STOP_MOVING, 0), T(TASK_FACE_ENEMY, 0), T(TASK_MELEE_ATTACK1, 0),
};
const Schedule_t slPrimaryMeleeAttack[] = {
	SCHED(tlPrimaryMeleeAttack1, COND_ATTACK, 0, "Primary Melee Attack")
};

static const Task_t tlSecondaryMeleeAttack1[] = {
	T(TASK_STOP_MOVING, 0), T(TASK_FACE_ENEMY, 0), T(TASK_MELEE_ATTACK2, 0),
};
const Schedule_t slSecondaryMeleeAttack[] = {
	SCHED(tlSecondaryMeleeAttack1, COND_ATTACK, 0, "Secondary Melee Attack")
};

static const Task_t tlSpecialAttack1[] = {
	T(TASK_STOP_MOVING, 0), T(TASK_FACE_ENEMY, 0), T(TASK_SPECIAL_ATTACK1, 0),
};
const Schedule_t slSpecialAttack1[] = {
	SCHED(tlSpecialAttack1, COND_ATTACK | bits_COND_NO_AMMO_LOADED | bits_COND_HEAR_SOUND,
		bits_SOUND_DANGER, "Special Attack1")
};

static const Task_t tlSpecialAttack2[] = {
	T(TASK_STOP_MOVING, 0), T(TASK_FACE_ENEMY, 0), T(TASK_SPECIAL_ATTACK2, 0),
};
const Schedule_t slSpecialAttack2[] = {
	SCHED(tlSpecialAttack2, COND_ATTACK | bits_COND_NO_AMMO_LOADED | bits_COND_HEAR_SOUND,
		bits_SOUND_DANGER, "Special Attack2")
};

static const Task_t tlChaseEnemy1[] = {
	T(TASK_SET_FAIL_SCHEDULE, SCHED_CHASE_ENEMY_FAILED), T(TASK_GET_PATH_TO_ENEMY, 0),
	T(TASK_RUN_PATH, 0), T(TASK_WAIT_FOR_MOVEMENT, 0),
};
const Schedule_t slChaseEnemy[] = {
	SCHED(tlChaseEnemy1, bits_COND_NEW_ENEMY | bits_COND_CAN_ATTACK | bits_COND_TASK_FAILED |
		bits_COND_HEAR_SOUND, bits_SOUND_DANGER, "Chase Enemy")
};

static const Task_t tlChaseEnemyFailed[] = {
	T(TASK_STOP_MOVING, 0), T(TASK_WAIT, 0.2), T(TASK_FIND_COVER_FROM_ENEMY, 0),
	T(TASK_RUN_PATH, 0), T(TASK_WAIT_FOR_MOVEMENT, 0), T(TASK_REMEMBER, bits_MEMORY_INCOVER),
	T(TASK_FACE_ENEMY, 0), T(TASK_WAIT, 1),
};
const Schedule_t slChaseEnemyFailed[] = {
	SCHED(tlChaseEnemyFailed, bits_COND_NEW_ENEMY | bits_COND_CAN_ATTACK | bits_COND_HEAR_SOUND,
		bits_SOUND_DANGER, "tlChaseEnemyFailed")
};

static const Task_t tlSmallFlinch[] = {
	T(TASK_REMEMBER, bits_MEMORY_FLINCHED), T(TASK_STOP_MOVING, 0), T(TASK_SMALL_FLINCH, 0),
};
const Schedule_t slSmallFlinch[] = { SCHED(tlSmallFlinch, 0, 0, "Small Flinch") };

static const Task_t tlDie1[] = { T(TASK_STOP_MOVING, 0), T(TASK_SOUND_DIE, 0), T(TASK_DIE, 0) };
const Schedule_t slDie[] = { SCHED(tlDie1, 0, 0, "Die") };

static const Task_t tlVictoryDance[] = {
	T(TASK_STOP_MOVING, 0), T(TASK_PLAY_SEQUENCE, ACT_VICTORY_DANCE), T(TASK_WAIT, 0),
};
const Schedule_t slVictoryDance[] = { SCHED(tlVictoryDance, 0, 0, "Victory Dance") };

static const Task_t tlBarnacleVictimGrab[] = {
	T(TASK_STOP_MOVING, 0), T(TASK_PLAY_SEQUENCE, ACT_BARNACLE_HIT),
	T(TASK_SET_ACTIVITY, ACT_BARNACLE_PULL), T(TASK_WAIT_INDEFINITE, 0),
};
const Schedule_t slBarnacleVictimGrab[] = { SCHED(tlBarnacleVictimGrab, 0, 0, "Barnacle Victim") };

static const Task_t tlBarnacleVictimChomp[] = {
	T(TASK_STOP_MOVING, 0), T(TASK_PLAY_SEQUENCE, ACT_BARNACLE_CHOMP),
	T(TASK_SET_ACTIVITY, ACT_BARNACLE_CHEW), T(TASK_WAIT_INDEFINITE, 0),
};
const Schedule_t slBarnacleVictimChomp[] = { SCHED(tlBarnacleVictimChomp, 0, 0, "Barnacle Chomp") };

static const Task_t tlError[] = { T(TASK_STOP_MOVING, 0), T(TASK_WAIT_INDEFINITE, 0) };
const Schedule_t slError[] = { SCHED(tlError, 0, 0, "Error") };

static const Task_t tlScriptedWalk[] = {
	T(TASK_WALK_TO_TARGET, TARGET_MOVE_SCRIPTED), T(TASK_WAIT_FOR_MOVEMENT, 0),
	T(TASK_PLANT_ON_SCRIPT, 0), T(TASK_FACE_SCRIPT, 0), T(TASK_FACE_IDEAL, 0),
	T(TASK_ENABLE_SCRIPT, 0), T(TASK_WAIT_FOR_SCRIPT, 0), T(TASK_PLAY_SCRIPT, 0),
};
const Schedule_t slWalkToScript[] = { SCHED(tlScriptedWalk, SCRIPT_BREAK_CONDITIONS, 0, "WalkToScript") };

static const Task_t tlScriptedRun[] = {
	T(TASK_RUN_TO_TARGET, TARGET_MOVE_SCRIPTED), T(TASK_WAIT_FOR_MOVEMENT, 0),
	T(TASK_PLANT_ON_SCRIPT, 0), T(TASK_FACE_SCRIPT, 0), T(TASK_FACE_IDEAL, 0),
	T(TASK_ENABLE_SCRIPT, 0), T(TASK_WAIT_FOR_SCRIPT, 0), T(TASK_PLAY_SCRIPT, 0),
};
const Schedule_t slRunToScript[] = { SCHED(tlScriptedRun, SCRIPT_BREAK_CONDITIONS, 0, "RunToScript") };

static const Task_t tlScriptedWait[] = {
	T(TASK_STOP_MOVING, 0), T(TASK_WAIT_FOR_SCRIPT, 0), T(TASK_PLAY_SCRIPT, 0),
};
const Schedule_t slWaitScript[] = { SCHED(tlScriptedWait, SCRIPT_BREAK_CONDITIONS, 0, "WaitForScript") };

static const Task_t tlScriptedFace[] = {
	T(TASK_STOP_MOVING, 0), T(TASK_FACE_SCRIPT, 0), T(TASK_FACE_IDEAL, 0),
	T(TASK_WAIT_FOR_SCRIPT, 0), T(TASK_PLAY_SCRIPT, 0),
};
const Schedule_t slFaceScript[] = { SCHED(tlScriptedFace, SCRIPT_BREAK_CONDITIONS, 0, "FaceScript") };

static const Task_t tlCower[] = { T(TASK_STOP_MOVING, 0), T(TASK_PLAY_SEQUENCE, ACT_COWER) };
const Schedule_t slCower[] = { SCHED(tlCower, 0, 0, "Cower") };

static const Task_t tlTakeCoverFromOrigin[] = {
	T(TASK_STOP_MOVING, 0), T(TASK_FIND_COVER_FROM_ORIGIN, 0), T(TASK_RUN_PATH, 0),
	T(TASK_WAIT_FOR_MOVEMENT, 0), T(TASK_REMEMBER, bits_MEMORY_INCOVER), T(TASK_TURN_LEFT, 179),
};
const Schedule_t slTakeCoverFromOrigin[] = {
	SCHED(tlTakeCoverFromOrigin, bits_COND_NEW_ENEMY, 0, "TakeCoverFromOrigin")
};

static const Task_t tlTakeCoverFromBestSound[] = {
	T(TASK_STOP_MOVING, 0), T(TASK_FIND_COVER_FROM_BEST_SOUND, 0), T(TASK_RUN_PATH, 0),
	T(TASK_WAIT_FOR_MOVEMENT, 0), T(TASK_REMEMBER, bits_MEMORY_INCOVER), T(TASK_TURN_LEFT, 179),
};
const Schedule_t slTakeCoverFromBestSound[] = {
	SCHED(tlTakeCoverFromBestSound, bits_COND_NEW_ENEMY, 0, "TakeCoverFromBestSound")
};

static const Task_t tlTakeCoverFromEnemy[] = {
	T(TASK_STOP_MOVING, 0), T(TASK_WAIT, 0.2), T(TASK_FIND_COVER_FROM_ENEMY, 0),
	T(TASK_RUN_PATH, 0), T(TASK_WAIT_FOR_MOVEMENT, 0), T(TASK_REMEMBER, bits_MEMORY_INCOVER),
	T(TASK_FACE_ENEMY, 0), T(TASK_WAIT, 1),
};
const Schedule_t slTakeCoverFromEnemy[] = {
	SCHED(tlTakeCoverFromEnemy, bits_COND_NEW_ENEMY, 0, "tlTakeCoverFromEnemy")
};

const Schedule_t *Mon_ScheduleError(void) { return &slError[0]; }

const Schedule_t *CBaseMonster_GetScheduleOfType(int e, int type)
{
	CBaseMonster	*m = MON(e);
	int		cine;

	switch (type)
	{
	case SCHED_AISCRIPT:
		cine = EHGet(m->m_pCine);
		if (cine < 0)
		{
			Mon_CineCleanup(e);
			return Mon_GetScheduleOfType(e, SCHED_IDLE_STAND);
		}
		switch (Script_MoveTo(cine))
		{
		case 0:
		case 4:
			return slWaitScript;
		case 1:
			return slWalkToScript;
		case 2:
			return slRunToScript;
		case 5:
			return slFaceScript;
		}
		break;
	case SCHED_IDLE_STAND:
		if (RANDOM_LONG(0, 14) == 0 && Mon_FCanActiveIdle(e))
			return &slActiveIdle[0];
		return &slIdleStand[0];
	case SCHED_IDLE_WALK: return &slIdleWalk[0];
	case SCHED_WAIT_TRIGGER: return &slIdleTrigger[0];
	case SCHED_WAKE_ANGRY: return &slWakeAngry[0];
	case SCHED_ALERT_FACE: return &slAlertFace[0];
	case SCHED_ALERT_STAND: return &slAlertStand[0];
	case SCHED_COMBAT_STAND: return &slCombatStand[0];
	case SCHED_COMBAT_FACE: return &slCombatFace[0];
	case SCHED_CHASE_ENEMY: return &slChaseEnemy[0];
	case SCHED_CHASE_ENEMY_FAILED: return &slFail[0];
	case SCHED_SMALL_FLINCH: return &slSmallFlinch[0];
	case SCHED_ALERT_SMALL_FLINCH: return &slAlertSmallFlinch[0];
	case SCHED_RELOAD: return &slReload[0];
	case SCHED_ARM_WEAPON: return &slArmWeapon[0];
	case SCHED_STANDOFF: return &slStandoff[0];
	case SCHED_RANGE_ATTACK1: return &slRangeAttack1[0];
	case SCHED_RANGE_ATTACK2: return &slRangeAttack2[0];
	case SCHED_MELEE_ATTACK1: return &slPrimaryMeleeAttack[0];
	case SCHED_MELEE_ATTACK2: return &slSecondaryMeleeAttack[0];
	case SCHED_SPECIAL_ATTACK1: return &slSpecialAttack1[0];
	case SCHED_SPECIAL_ATTACK2: return &slSpecialAttack2[0];
	case SCHED_TAKE_COVER_FROM_BEST_SOUND: return &slTakeCoverFromBestSound[0];
	case SCHED_TAKE_COVER_FROM_ENEMY: return &slTakeCoverFromEnemy[0];
	case SCHED_COWER: return &slCower[0];
	case SCHED_AMBUSH: return &slAmbush[0];
	case SCHED_BARNACLE_VICTIM_GRAB: return &slBarnacleVictimGrab[0];
	case SCHED_BARNACLE_VICTIM_CHOMP: return &slBarnacleVictimChomp[0];
	case SCHED_INVESTIGATE_SOUND: return &slInvestigateSound[0];
	case SCHED_DIE: return &slDie[0];
	case SCHED_TAKE_COVER_FROM_ORIGIN: return &slTakeCoverFromOrigin[0];
	case SCHED_VICTORY_DANCE: return &slVictoryDance[0];
	case SCHED_FAIL: return slFail;
	default: return &slIdleStand[0];
	}
	/* HLSDK returns NULL for an unknown m_fMoveTo (MaintainSchedule's
	 * ChangeSchedule would then fault); fall back to the error schedule */
	return &slError[0];
}
