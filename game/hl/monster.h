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
 * monster.h -- CBaseMonster, CSquadMonster and CTalkMonster in C (HLSDK
 * dlls/basemonster.h, monsters.h, schedule.h, activity.h, defaultai.h,
 * squadmonster.h, talkmonster.h, soundent.h, scripted.h, scriptevent.h).
 *
 * Layout: flat member macros after GameCore's CBaseToggle chain
 * (game/hl/cbase.h). Entity references held across thinks are ent_h
 * (EH_NONE = NULL EHANDLE). HLSDK virtual functions a monster class
 * overrides live in its mon_vt_t (game_class_t.ext); NULL slots run the
 * CBaseMonster (or CTalkMonster / CSquadMonster) version. Every method
 * takes the entity slot e; MON(e) is its private block.
 *
 * Strings: schedule names stay for debug prints; sentence groups are
 * res.def GROUP ids, sentences rtb SENTENCE ids; scripted sequence names
 * are kept as text in the script's block and resolved to sequence
 * indices once, when the script takes its monster.
 */
#ifndef HL64_GAME_MONSTER_H
#define HL64_GAME_MONSTER_H

#include <stdint.h>

#include "game/hl/cbase.h"
#include "game/hl/gib.h"
#include "game/hl/util.h"
#include "game/studio/anim.h"
#include "hl64/studio.h"
#include "hl64/game.h"

/* ---- vector helpers beyond util.h ----------------------------------------- */

static inline float mv_dist(const float a[3], const float b[3])
{ float d[3]; VecSub(a, b, d); return VecLen(d); }
static inline float mv_dist2d(const float a[3], const float b[3])
{ float d[3]; VecSub(a, b, d); return VecLen2D(d); }

void	mv_norm2d(float d[3], const float a[3]);	/* Vector2D Normalize, d[2] = 0 */

/* ---- activity.h ---------------------------------------------------------- */

typedef enum {
	ACT_RESET = 0, ACT_IDLE = 1, ACT_GUARD, ACT_WALK, ACT_RUN, ACT_FLY, ACT_SWIM, ACT_HOP,
	ACT_LEAP, ACT_FALL, ACT_LAND, ACT_STRAFE_LEFT, ACT_STRAFE_RIGHT, ACT_ROLL_LEFT,
	ACT_ROLL_RIGHT, ACT_TURN_LEFT, ACT_TURN_RIGHT, ACT_CROUCH, ACT_CROUCHIDLE, ACT_STAND,
	ACT_USE, ACT_SIGNAL1, ACT_SIGNAL2, ACT_SIGNAL3, ACT_TWITCH, ACT_COWER, ACT_SMALL_FLINCH,
	ACT_BIG_FLINCH, ACT_RANGE_ATTACK1, ACT_RANGE_ATTACK2, ACT_MELEE_ATTACK1,
	ACT_MELEE_ATTACK2, ACT_RELOAD, ACT_ARM, ACT_DISARM, ACT_EAT, ACT_DIESIMPLE,
	ACT_DIEBACKWARD, ACT_DIEFORWARD, ACT_DIEVIOLENT, ACT_BARNACLE_HIT, ACT_BARNACLE_PULL,
	ACT_BARNACLE_CHOMP, ACT_BARNACLE_CHEW, ACT_SLEEP, ACT_INSPECT_FLOOR, ACT_INSPECT_WALL,
	ACT_IDLE_ANGRY, ACT_WALK_HURT, ACT_RUN_HURT, ACT_HOVER, ACT_GLIDE, ACT_FLY_LEFT,
	ACT_FLY_RIGHT, ACT_DETECT_SCENT, ACT_SNIFF, ACT_BITE, ACT_THREAT_DISPLAY,
	ACT_FEAR_DISPLAY, ACT_EXCITED, ACT_SPECIAL_ATTACK1, ACT_SPECIAL_ATTACK2,
	ACT_COMBAT_IDLE, ACT_WALK_SCARED, ACT_RUN_SCARED, ACT_VICTORY_DANCE, ACT_DIE_HEADSHOT,
	ACT_DIE_CHESTSHOT, ACT_DIE_GUTSHOT, ACT_DIE_BACKSHOT, ACT_FLINCH_HEAD, ACT_FLINCH_CHEST,
	ACT_FLINCH_STOMACH, ACT_FLINCH_LEFTARM, ACT_FLINCH_RIGHTARM, ACT_FLINCH_LEFTLEG,
	ACT_FLINCH_RIGHTLEG
} Activity;

/* ---- util.h MONSTERSTATE ------------------------------------------------ */

#ifndef MONSTERSTATE_NONE_DEFINED
#define MONSTERSTATE_NONE_DEFINED
typedef enum {
	MONSTERSTATE_NONE = 0, MONSTERSTATE_IDLE, MONSTERSTATE_COMBAT, MONSTERSTATE_ALERT,
	MONSTERSTATE_HUNT, MONSTERSTATE_PRONE, MONSTERSTATE_SCRIPT, MONSTERSTATE_PLAYDEAD,
	MONSTERSTATE_DEAD
} MONSTERSTATE;
#endif

/* ---- monsters.h ------------------------------------------------------------ */

#define LOCALMOVE_INVALID			0
#define LOCALMOVE_INVALID_DONT_TRIANGULATE	1
#define LOCALMOVE_VALID				2

#define HITGROUP_GENERIC	0
#define HITGROUP_HEAD		1
#define HITGROUP_CHEST		2
#define HITGROUP_STOMACH	3
#define HITGROUP_LEFTARM	4
#define HITGROUP_RIGHTARM	5
#define HITGROUP_LEFTLEG	6
#define HITGROUP_RIGHTLEG	7

#define SF_MONSTER_WAIT_TILL_SEEN	1
#define SF_MONSTER_GAG			2
#define SF_MONSTER_HITMONSTERCLIP	4
#define SF_MONSTER_PRISONER		16
#define SF_MONSTER_WAIT_FOR_SCRIPT	128
#define SF_MONSTER_PREDISASTER		256
#define SF_MONSTER_FADECORPSE		512
#define SF_MONSTER_FALL_TO_GROUND	0x80000000u
#define SF_MONSTER_TURRET_AUTOACTIVATE	32
#define SF_MONSTER_TURRET_STARTINACTIVE	64
#define SF_MONSTER_WAIT_UNTIL_PROVOKED	64

#define MOVE_START_TURN_DIST	64
#define MOVE_STUCK_DIST		32
#define MOVE_NORMAL		0
#define MOVE_STRAFE		1

#define R_AL	-2
#define R_FR	-1
#define R_NO	0
#define R_DL	1
#define R_HT	2
#define R_NM	3

#define MEMORY_CLEAR			0
#define bits_MEMORY_PROVOKED		(1 << 0)
#define bits_MEMORY_INCOVER		(1 << 1)
#define bits_MEMORY_SUSPICIOUS		(1 << 2)
#define bits_MEMORY_PATH_FINISHED	(1 << 3)
#define bits_MEMORY_ON_PATH		(1 << 4)
#define bits_MEMORY_MOVE_FAILED		(1 << 5)
#define bits_MEMORY_FLINCHED		(1 << 6)
#define bits_MEMORY_KILLED		(1 << 7)
#define bits_MEMORY_CUSTOM4		(1 << 28)
#define bits_MEMORY_CUSTOM3		(1 << 29)
#define bits_MEMORY_CUSTOM2		(1 << 30)
#define bits_MEMORY_CUSTOM1		(1u << 31)

enum {
	AITRIGGER_NONE = 0, AITRIGGER_SEEPLAYER_ANGRY_AT_PLAYER, AITRIGGER_TAKEDAMAGE,
	AITRIGGER_HALFHEALTH, AITRIGGER_DEATH, AITRIGGER_SQUADMEMBERDIE,
	AITRIGGER_SQUADLEADERDIE, AITRIGGER_HEARWORLD, AITRIGGER_HEARPLAYER,
	AITRIGGER_HEARCOMBAT, AITRIGGER_SEEPLAYER_UNCONDITIONAL,
	AITRIGGER_SEEPLAYER_NOT_IN_COMBAT
};

/* cbase.h capabilities */
#define bits_CAP_DUCK		(1 << 0)
#define bits_CAP_JUMP		(1 << 1)
#define bits_CAP_STRAFE		(1 << 2)
#define bits_CAP_SQUAD		(1 << 3)
#define bits_CAP_SWIM		(1 << 4)
#define bits_CAP_CLIMB		(1 << 5)
#define bits_CAP_USE		(1 << 6)
#define bits_CAP_HEAR		(1 << 7)
#define bits_CAP_AUTO_DOORS	(1 << 8)
#define bits_CAP_OPEN_DOORS	(1 << 9)
#define bits_CAP_TURN_HEAD	(1 << 10)
#define bits_CAP_RANGE_ATTACK1	(1 << 11)
#define bits_CAP_RANGE_ATTACK2	(1 << 12)
#define bits_CAP_MELEE_ATTACK1	(1 << 13)
#define bits_CAP_MELEE_ATTACK2	(1 << 14)
#define bits_CAP_FLY		(1 << 15)
#define bits_CAP_DOORS_GROUP	(bits_CAP_USE | bits_CAP_AUTO_DOORS | bits_CAP_OPEN_DOORS)

/* util.h view fields */
#define VIEW_FIELD_FULL		(-1.0f)
#define VIEW_FIELD_WIDE		(-0.7f)
#define VIEW_FIELD_NARROW	0.7f
#define VIEW_FIELD_ULTRA_NARROW	0.9f

#ifndef ROUTE_SIZE
#define ROUTE_SIZE		8
#endif
#ifndef MAX_OLD_ENEMIES
#define MAX_OLD_ENEMIES		4
#endif
#ifndef CDMG_TIMEBASED
#define CDMG_TIMEBASED		8
#endif

/* ---- schedule.h ------------------------------------------------------------ */

#define TASKSTATUS_NEW			0
#define TASKSTATUS_RUNNING		1
#define TASKSTATUS_RUNNING_MOVEMENT	2
#define TASKSTATUS_RUNNING_TASK		3
#define TASKSTATUS_COMPLETE		4

typedef enum {
	SCHED_NONE = 0, SCHED_IDLE_STAND, SCHED_IDLE_WALK, SCHED_WAKE_ANGRY, SCHED_WAKE_CALLED,
	SCHED_ALERT_FACE, SCHED_ALERT_SMALL_FLINCH, SCHED_ALERT_BIG_FLINCH, SCHED_ALERT_STAND,
	SCHED_INVESTIGATE_SOUND, SCHED_COMBAT_FACE, SCHED_COMBAT_STAND, SCHED_CHASE_ENEMY,
	SCHED_CHASE_ENEMY_FAILED, SCHED_VICTORY_DANCE, SCHED_TARGET_FACE, SCHED_TARGET_CHASE,
	SCHED_SMALL_FLINCH, SCHED_TAKE_COVER_FROM_ENEMY, SCHED_TAKE_COVER_FROM_BEST_SOUND,
	SCHED_TAKE_COVER_FROM_ORIGIN, SCHED_COWER, SCHED_MELEE_ATTACK1, SCHED_MELEE_ATTACK2,
	SCHED_RANGE_ATTACK1, SCHED_RANGE_ATTACK2, SCHED_SPECIAL_ATTACK1, SCHED_SPECIAL_ATTACK2,
	SCHED_STANDOFF, SCHED_ARM_WEAPON, SCHED_RELOAD, SCHED_GUARD, SCHED_AMBUSH, SCHED_DIE,
	SCHED_WAIT_TRIGGER, SCHED_FOLLOW, SCHED_SLEEP, SCHED_WAKE, SCHED_BARNACLE_VICTIM_GRAB,
	SCHED_BARNACLE_VICTIM_CHOMP, SCHED_AISCRIPT, SCHED_FAIL,
	LAST_COMMON_SCHEDULE
} SCHEDULE_TYPE;

typedef enum {
	TASK_INVALID = 0, TASK_WAIT, TASK_WAIT_FACE_ENEMY, TASK_WAIT_PVS, TASK_SUGGEST_STATE,
	TASK_WALK_TO_TARGET, TASK_RUN_TO_TARGET, TASK_MOVE_TO_TARGET_RANGE,
	TASK_GET_PATH_TO_ENEMY, TASK_GET_PATH_TO_ENEMY_LKP, TASK_GET_PATH_TO_ENEMY_CORPSE,
	TASK_GET_PATH_TO_LEADER, TASK_GET_PATH_TO_SPOT, TASK_GET_PATH_TO_TARGET,
	TASK_GET_PATH_TO_HINTNODE, TASK_GET_PATH_TO_LASTPOSITION, TASK_GET_PATH_TO_BESTSOUND,
	TASK_GET_PATH_TO_BESTSCENT, TASK_RUN_PATH, TASK_WALK_PATH, TASK_STRAFE_PATH,
	TASK_CLEAR_MOVE_WAIT, TASK_STORE_LASTPOSITION, TASK_CLEAR_LASTPOSITION,
	TASK_PLAY_ACTIVE_IDLE, TASK_FIND_HINTNODE, TASK_CLEAR_HINTNODE, TASK_SMALL_FLINCH,
	TASK_FACE_IDEAL, TASK_FACE_ROUTE, TASK_FACE_ENEMY, TASK_FACE_HINTNODE, TASK_FACE_TARGET,
	TASK_FACE_LASTPOSITION, TASK_RANGE_ATTACK1, TASK_RANGE_ATTACK2, TASK_MELEE_ATTACK1,
	TASK_MELEE_ATTACK2, TASK_RELOAD, TASK_RANGE_ATTACK1_NOTURN, TASK_RANGE_ATTACK2_NOTURN,
	TASK_MELEE_ATTACK1_NOTURN, TASK_MELEE_ATTACK2_NOTURN, TASK_RELOAD_NOTURN,
	TASK_SPECIAL_ATTACK1, TASK_SPECIAL_ATTACK2, TASK_CROUCH, TASK_STAND, TASK_GUARD,
	TASK_STEP_LEFT, TASK_STEP_RIGHT, TASK_STEP_FORWARD, TASK_STEP_BACK, TASK_DODGE_LEFT,
	TASK_DODGE_RIGHT, TASK_SOUND_ANGRY, TASK_SOUND_DEATH, TASK_SET_ACTIVITY,
	TASK_SET_SCHEDULE, TASK_SET_FAIL_SCHEDULE, TASK_CLEAR_FAIL_SCHEDULE, TASK_PLAY_SEQUENCE,
	TASK_PLAY_SEQUENCE_FACE_ENEMY, TASK_PLAY_SEQUENCE_FACE_TARGET, TASK_SOUND_IDLE,
	TASK_SOUND_WAKE, TASK_SOUND_PAIN, TASK_SOUND_DIE, TASK_FIND_COVER_FROM_BEST_SOUND,
	TASK_FIND_COVER_FROM_ENEMY, TASK_FIND_LATERAL_COVER_FROM_ENEMY,
	TASK_FIND_NODE_COVER_FROM_ENEMY, TASK_FIND_NEAR_NODE_COVER_FROM_ENEMY,
	TASK_FIND_FAR_NODE_COVER_FROM_ENEMY, TASK_FIND_COVER_FROM_ORIGIN, TASK_EAT, TASK_DIE,
	TASK_WAIT_FOR_SCRIPT, TASK_PLAY_SCRIPT, TASK_ENABLE_SCRIPT, TASK_PLANT_ON_SCRIPT,
	TASK_FACE_SCRIPT, TASK_WAIT_RANDOM, TASK_WAIT_INDEFINITE, TASK_STOP_MOVING,
	TASK_TURN_LEFT, TASK_TURN_RIGHT, TASK_REMEMBER, TASK_FORGET, TASK_WAIT_FOR_MOVEMENT,
	LAST_COMMON_TASK
} SHARED_TASKS;

enum { TARGET_MOVE_NORMAL = 0, TARGET_MOVE_SCRIPTED = 1 };

typedef struct {
	int		iTask;
	float		flData;
} Task_t;

typedef struct {
	const Task_t	*pTasklist;
	int		cTasks;
	int		iInterruptMask;
	int		iSoundMask;
	const char	*pName;
} Schedule_t;

#define ARRAYSIZE(a)	((int)(sizeof(a) / sizeof((a)[0])))
/* Schedule_t slX[] = { SCHED(tlX, mask, sounds, "X") } */
#define SCHED(tl, mask, snd, name)	{ tl, ARRAYSIZE(tl), (int)(mask), (int)(snd), name }

typedef struct {
	float		vecLocation[3];
	int		iType;
} WayPoint_t;

#define bits_MF_TO_TARGETENT	(1 << 0)
#define bits_MF_TO_ENEMY	(1 << 1)
#define bits_MF_TO_COVER	(1 << 2)
#define bits_MF_TO_DETOUR	(1 << 3)
#define bits_MF_TO_PATHCORNER	(1 << 4)
#define bits_MF_TO_NODE		(1 << 5)
#define bits_MF_TO_LOCATION	(1 << 6)
#define bits_MF_IS_GOAL		(1 << 7)
#define bits_MF_DONT_SIMPLIFY	(1 << 8)
#define bits_MF_NOT_TO_MASK	(bits_MF_IS_GOAL | bits_MF_DONT_SIMPLIFY)

#define MOVEGOAL_NONE		0
#define MOVEGOAL_TARGETENT	bits_MF_TO_TARGETENT
#define MOVEGOAL_ENEMY		bits_MF_TO_ENEMY
#define MOVEGOAL_PATHCORNER	bits_MF_TO_PATHCORNER
#define MOVEGOAL_LOCATION	bits_MF_TO_LOCATION
#define MOVEGOAL_NODE		bits_MF_TO_NODE

#define bits_COND_NO_AMMO_LOADED	(1 << 0)
#define bits_COND_SEE_HATE		(1 << 1)
#define bits_COND_SEE_FEAR		(1 << 2)
#define bits_COND_SEE_DISLIKE		(1 << 3)
#define bits_COND_SEE_ENEMY		(1 << 4)
#define bits_COND_ENEMY_OCCLUDED	(1 << 5)
#define bits_COND_SMELL_FOOD		(1 << 6)
#define bits_COND_ENEMY_TOOFAR		(1 << 7)
#define bits_COND_LIGHT_DAMAGE		(1 << 8)
#define bits_COND_HEAVY_DAMAGE		(1 << 9)
#define bits_COND_CAN_RANGE_ATTACK1	(1 << 10)
#define bits_COND_CAN_MELEE_ATTACK1	(1 << 11)
#define bits_COND_CAN_RANGE_ATTACK2	(1 << 12)
#define bits_COND_CAN_MELEE_ATTACK2	(1 << 13)
#define bits_COND_PROVOKED		(1 << 15)
#define bits_COND_NEW_ENEMY		(1 << 16)
#define bits_COND_HEAR_SOUND		(1 << 17)
#define bits_COND_SMELL			(1 << 18)
#define bits_COND_ENEMY_FACING_ME	(1 << 19)
#define bits_COND_ENEMY_DEAD		(1 << 20)
#define bits_COND_SEE_CLIENT		(1 << 21)
#define bits_COND_SEE_NEMESIS		(1 << 22)
#define bits_COND_SPECIAL1		(1 << 28)
#define bits_COND_SPECIAL2		(1 << 29)
#define bits_COND_TASK_FAILED		(1 << 30)
#define bits_COND_SCHEDULE_DONE		((int)(1u << 31))
#define bits_COND_ALL_SPECIAL		(bits_COND_SPECIAL1 | bits_COND_SPECIAL2)
#define bits_COND_CAN_ATTACK		(bits_COND_CAN_RANGE_ATTACK1 | bits_COND_CAN_MELEE_ATTACK1 | \
					 bits_COND_CAN_RANGE_ATTACK2 | bits_COND_CAN_MELEE_ATTACK2)

/* defaultai.c schedules */
extern const Schedule_t	slFail[];
extern const Schedule_t	slIdleStand[];
extern const Schedule_t	slIdleTrigger[];
extern const Schedule_t	slIdleWalk[];
extern const Schedule_t	slAmbush[];
extern const Schedule_t	slActiveIdle[];
extern const Schedule_t	slWakeAngry[];
extern const Schedule_t	slAlertFace[];
extern const Schedule_t	slAlertSmallFlinch[];
extern const Schedule_t	slAlertStand[];
extern const Schedule_t	slInvestigateSound[];
extern const Schedule_t	slCombatStand[];
extern const Schedule_t	slCombatFace[];
extern const Schedule_t	slStandoff[];
extern const Schedule_t	slArmWeapon[];
extern const Schedule_t	slReload[];
extern const Schedule_t	slRangeAttack1[];
extern const Schedule_t	slRangeAttack2[];
extern const Schedule_t	slPrimaryMeleeAttack[];
extern const Schedule_t	slSecondaryMeleeAttack[];
extern const Schedule_t	slSpecialAttack1[];
extern const Schedule_t	slSpecialAttack2[];
extern const Schedule_t	slChaseEnemy[];
extern const Schedule_t	slChaseEnemyFailed[];
extern const Schedule_t	slSmallFlinch[];
extern const Schedule_t	slDie[];
extern const Schedule_t	slVictoryDance[];
extern const Schedule_t	slBarnacleVictimGrab[];
extern const Schedule_t	slBarnacleVictimChomp[];
extern const Schedule_t	slError[];
extern const Schedule_t	slWalkToScript[];
extern const Schedule_t	slRunToScript[];
extern const Schedule_t	slWaitScript[];
extern const Schedule_t	slFaceScript[];
extern const Schedule_t	slCower[];
extern const Schedule_t	slTakeCoverFromOrigin[];
extern const Schedule_t	slTakeCoverFromBestSound[];
extern const Schedule_t	slTakeCoverFromEnemy[];
const Schedule_t *Mon_ScheduleError(void);

/* ---- soundent.h ------------------------------------------------------------ */

#define MAX_WORLD_SOUNDS	64
#define bits_SOUND_NONE		0
#define bits_SOUND_COMBAT	(1 << 0)
#define bits_SOUND_WORLD	(1 << 1)
#define bits_SOUND_PLAYER	(1 << 2)
#define bits_SOUND_CARCASS	(1 << 3)
#define bits_SOUND_MEAT		(1 << 4)
#define bits_SOUND_DANGER	(1 << 5)
#define bits_SOUND_GARBAGE	(1 << 6)
#define bits_ALL_SOUNDS		((int)0xFFFFFFFF)
#define SOUNDLIST_EMPTY		-1
#define SOUND_NEVER_EXPIRE	-1

typedef struct {
	float		m_vecOrigin[3];
	int		m_iType;
	int		m_iVolume;
	float		m_flExpireTime;
	int		m_iNext;
	int		m_iNextAudible;
} CSound;

/* CSoundEnt (game/hl/aisound.c): the world's sound list. */
void	CSoundEnt_Init(void);		/* New Game / map start */
void	CSoundEnt_Think(void);		/* game_start_frame, every 0.3 s as HLSDK */
void	CSoundEnt_InsertSound(int iType, const float org[3], int iVolume, float flDuration);
int	CSoundEnt_ActiveList(void);
CSound	*CSoundEnt_SoundPointerForIndex(int i);
int	CSoundEnt_ClientSoundIndex(int e);
static inline int CSound_FIsSound(const CSound *s)
{
	return (s->m_iType & (bits_SOUND_COMBAT | bits_SOUND_WORLD | bits_SOUND_PLAYER |
		bits_SOUND_DANGER)) != 0;
}
static inline int CSound_FIsScent(const CSound *s)
{
	return (s->m_iType & (bits_SOUND_CARCASS | bits_SOUND_MEAT | bits_SOUND_GARBAGE)) != 0;
}

/* ---- monsterevent.h / scriptevent.h ------------------------------------------ */

typedef struct {
	int		event;
	const char	*options;
	res_t		res;		/* cooked option resource (gs_anim_event_res) */
} MonsterEvent_t;

#define MONSTER_EVENT_BODYDROP_LIGHT	2001
#define MONSTER_EVENT_BODYDROP_HEAVY	2002
#define MONSTER_EVENT_SWISHSOUND	2010

#define SCRIPT_EVENT_DEAD		1000
#define SCRIPT_EVENT_NOINTERRUPT	1001
#define SCRIPT_EVENT_CANINTERRUPT	1002
#define SCRIPT_EVENT_FIREEVENT		1003
#define SCRIPT_EVENT_SOUND		1004
#define SCRIPT_EVENT_SENTENCE		1005
#define SCRIPT_EVENT_INAIR		1006
#define SCRIPT_EVENT_ENDANIMATION	1007
#define SCRIPT_EVENT_SOUND_VOICE	1008
#define SCRIPT_EVENT_SENTENCE_RND1	1009
#define SCRIPT_EVENT_NOT_DEAD		1010

/* ---- scripted.h ------------------------------------------------------------------ */

#define SF_SCRIPT_WAITTILLSEEN		1
#define SF_SCRIPT_EXITAGITATED		2
#define SF_SCRIPT_REPEATABLE		4
#define SF_SCRIPT_LEAVECORPSE		8
#define SF_SCRIPT_NOINTERRUPT		32
#define SF_SCRIPT_OVERRIDESTATE		64
#define SF_SCRIPT_NOSCRIPTMOVEMENT	128
#define SCRIPT_BREAK_CONDITIONS		(bits_COND_LIGHT_DAMAGE | bits_COND_HEAVY_DAMAGE)

enum { SS_INTERRUPT_IDLE = 0, SS_INTERRUPT_BY_NAME, SS_INTERRUPT_AI };

#define SCRIPT_FINISHSCHED_DEFAULT	0
#define SCRIPT_FINISHSCHED_AMBUSH	1

typedef enum {
	SCRIPT_PLAYING = 0, SCRIPT_WAIT, SCRIPT_CLEANUP, SCRIPT_WALK_TO_MARK, SCRIPT_RUN_TO_MARK
} SCRIPTSTATE;

/* ---- CBaseMonster ------------------------------------------------------------------ */

#define CBASEMONSTER_FIELDS \
	int		m_afConditions; \
	ent_h		m_hEnemy; \
	ent_h		m_hTargetEnt; \
	ent_h		m_hOldEnemy[MAX_OLD_ENEMIES]; \
	float		m_vecOldEnemy[MAX_OLD_ENEMIES][3]; \
	float		m_flFieldOfView; \
	float		m_flWaitFinished; \
	float		m_flMoveWaitFinished; \
	int		m_Activity; \
	int		m_IdealActivity; \
	int		m_LastHitGroup; \
	int		m_MonsterState; \
	int		m_IdealMonsterState; \
	int		m_iTaskStatus; \
	const Schedule_t *m_pSchedule; \
	int		m_iScheduleIndex; \
	WayPoint_t	m_Route[ROUTE_SIZE]; \
	int		m_movementGoal; \
	int		m_iRouteIndex; \
	float		m_moveWaitTime; \
	float		m_vecMoveGoal[3]; \
	int		m_movementActivity; \
	int		m_iAudibleList; \
	int		m_afSoundTypes; \
	float		m_vecLastPosition[3]; \
	int		m_iHintNode; \
	int		m_afMemory; \
	int		m_iMaxHealth; \
	float		m_vecEnemyLKP[3]; \
	int		m_cAmmoLoaded; \
	int		m_afCapability; \
	float		m_flNextAttack; \
	int		m_bitsDamageType; \
	uint8_t		m_rgbTimeBasedDamage[CDMG_TIMEBASED]; \
	int		m_lastDamageAmount; \
	int		m_bloodColor; \
	int		m_failSchedule; \
	float		m_flHungryTime; \
	float		m_flDistTooFar; \
	float		m_flDistLook; \
	int		m_iTriggerCondition; \
	name_t		m_iszTriggerTarget; \
	uint16_t	m_monPad; \
	float		m_HackedGunPos[3]; \
	int		m_scriptState; \
	ent_h		m_pCine; \
	float		m_flLastYawTime; \
	gid_t16		m_makerGid;	/* monstermaker record of a child, GID_NONE */ \
	uint16_t	m_monPad2;

typedef struct {
	CBASEENTITY_FIELDS
	CBASEDELAY_FIELDS
	CBASEANIMATING_FIELDS
	CBASETOGGLE_FIELDS
	CBASEMONSTER_FIELDS
} CBaseMonster;

#define MON(e)		((CBaseMonster *)ent_priv(e))

/* ---- CSquadMonster (squadmonster.h) ------------------------------------------------- */

#define SF_SQUADMONSTER_LEADER		32
#define bits_NO_SLOT			0
#define bits_SLOT_HGRUNT_ENGAGE1	(1 << 0)
#define bits_SLOT_HGRUNT_ENGAGE2	(1 << 1)
#define bits_SLOTS_HGRUNT_ENGAGE	(bits_SLOT_HGRUNT_ENGAGE1 | bits_SLOT_HGRUNT_ENGAGE2)
#define bits_SLOT_HGRUNT_GRENADE1	(1 << 2)
#define bits_SLOT_HGRUNT_GRENADE2	(1 << 3)
#define bits_SLOTS_HGRUNT_GRENADE	(bits_SLOT_HGRUNT_GRENADE1 | bits_SLOT_HGRUNT_GRENADE2)
#define bits_SLOT_HOUND_ATTACK1		(1 << 7)
#define bits_SLOT_HOUND_ATTACK2		(1 << 8)
#define bits_SLOT_HOUND_ATTACK3		(1 << 9)
#define bits_SLOTS_HOUND_ATTACK		(bits_SLOT_HOUND_ATTACK1 | bits_SLOT_HOUND_ATTACK2 | \
					 bits_SLOT_HOUND_ATTACK3)
#define bits_SLOT_SQUAD_SPLIT		(1 << 10)
#define NUM_SLOTS			11
#define MAX_SQUAD_MEMBERS		5

#define CSQUADMONSTER_FIELDS \
	ent_h		m_hSquadLeader; \
	ent_h		m_hSquadMember[MAX_SQUAD_MEMBERS - 1]; \
	int		m_afSquadSlots; \
	float		m_flLastEnemySightTime; \
	int		m_fEnemyEluded; \
	int		m_iMySlot;

typedef struct {
	CBASEENTITY_FIELDS
	CBASEDELAY_FIELDS
	CBASEANIMATING_FIELDS
	CBASETOGGLE_FIELDS
	CBASEMONSTER_FIELDS
	CSQUADMONSTER_FIELDS
} CSquadMonster;

#define SQM(e)		((CSquadMonster *)ent_priv(e))

/* ---- CTalkMonster (talkmonster.h) ------------------------------------------------------ */

#define TALKRANGE_MIN		500.0f
#define TLK_STARE_DIST		128
#define bit_saidDamageLight	(1 << 0)
#define bit_saidDamageMedium	(1 << 1)
#define bit_saidDamageHeavy	(1 << 2)
#define bit_saidHelloPlayer	(1 << 3)
#define bit_saidWoundLight	(1 << 4)
#define bit_saidWoundHeavy	(1 << 5)
#define bit_saidHeard		(1 << 6)
#define bit_saidSmelled		(1 << 7)
#define TLK_CFRIENDS		3

typedef enum {
	TLK_ANSWER = 0, TLK_QUESTION, TLK_IDLE, TLK_STARE, TLK_USE, TLK_UNUSE, TLK_STOP,
	TLK_NOSHOOT, TLK_HELLO, TLK_PHELLO, TLK_PIDLE, TLK_PQUESTION, TLK_PLHURT1, TLK_PLHURT2,
	TLK_PLHURT3, TLK_SMELL, TLK_WOUND, TLK_MORTAL, TLK_CGROUPS
} TALKGROUPNAMES;

enum {
	SCHED_CANT_FOLLOW = LAST_COMMON_SCHEDULE + 1, SCHED_MOVE_AWAY, SCHED_MOVE_AWAY_FOLLOW,
	SCHED_MOVE_AWAY_FAIL, LAST_TALKMONSTER_SCHEDULE
};

enum {
	TASK_CANT_FOLLOW = LAST_COMMON_TASK + 1, TASK_MOVE_AWAY_PATH, TASK_WALK_PATH_FOR_UNITS,
	TASK_TLK_RESPOND, TASK_TLK_SPEAK, TASK_TLK_HELLO, TASK_TLK_HEADRESET,
	TASK_TLK_STOPSHOOTING, TASK_TLK_STARE, TASK_TLK_LOOK_AT_CLIENT, TASK_TLK_CLIENT_STARE,
	TASK_TLK_EYECONTACT, TASK_TLK_IDEALYAW, TASK_FACE_PLAYER, LAST_TALKMONSTER_TASK
};

#define bits_COND_CLIENT_PUSH	bits_COND_SPECIAL1
#define bits_COND_CLIENT_UNSEEN	bits_COND_SPECIAL2

/* m_szGrp entries are GROUP res_t (0 = none: RES_NONE). */
#define CTALKMONSTER_FIELDS \
	int		m_bitsSaid; \
	int		m_nSpeak; \
	int		m_voicePitch; \
	res_t		m_szGrp[TLK_CGROUPS]; \
	float		m_useTime; \
	res_t		m_iszUse; \
	res_t		m_iszUnUse; \
	float		m_flLastSaidSmelled; \
	float		m_flStopTalkTime; \
	ent_h		m_hTalkTarget;

typedef struct {
	CBASEENTITY_FIELDS
	CBASEDELAY_FIELDS
	CBASEANIMATING_FIELDS
	CBASETOGGLE_FIELDS
	CBASEMONSTER_FIELDS
	CTALKMONSTER_FIELDS
} CTalkMonster;

#define TALK(e)		((CTalkMonster *)ent_priv(e))

/* ---- the monster virtual table (game_class_t.ext) -------------------------------- */

typedef struct mon_vt_s {
	/* CBaseMonster virtuals */
	void		(*Look)(int e, int iDistance);
	void		(*RunAI)(int e);
	void		(*MonsterInit)(int e);
	void		(*MonsterInitDead)(int e);
	void		(*BecomeDead)(int e);
	void		(*StartMonster)(int e);
	int		(*BestVisibleEnemy)(int e);
	int		(*FInViewCone)(int e, int other);
	void		(*HandleAnimEvent)(int e, const MonsterEvent_t *ev);
	int		(*CheckLocalMove)(int e, const float start[3], const float end[3], int target,
				float *pflDist);
	void		(*Move)(int e, float flInterval);
	void		(*MoveExecute)(int e, int target, const float dir[3], float flInterval);
	int		(*ShouldAdvanceRoute)(int e, float flWaypointDist);
	int		(*GetStoppedActivity)(int e);
	void		(*Stop)(int e);
	int		(*CheckRangeAttack1)(int e, float flDot, float flDist);
	int		(*CheckRangeAttack2)(int e, float flDot, float flDist);
	int		(*CheckMeleeAttack1)(int e, float flDot, float flDist);
	int		(*CheckMeleeAttack2)(int e, float flDot, float flDist);
	void		(*StartTask)(int e, const Task_t *t);
	void		(*RunTask)(int e, const Task_t *t);
	const Schedule_t *(*GetScheduleOfType)(int e, int type);
	const Schedule_t *(*GetSchedule)(int e);
	void		(*ScheduleChange)(int e);
	int		(*CanPlaySequence)(int e, int fDisregardState, int interruptLevel);
	int		(*CanPlaySentence)(int e, int fDisregardState);
	void		(*PlaySentence)(int e, res_t sentence, float duration, float volume,
				float attenuation);
	void		(*PlayScriptedSentence)(int e, res_t sentence, float duration, float volume,
				float attenuation, int bConcurrent, int listener);
	int		(*IsAllowedToSpeak)(int e);
	int		(*GetIdealState)(int e);
	void		(*SetActivity)(int e, int act);
	int		(*CheckEnemy)(int e, int enemy);
	int		(*FTriangulate)(int e, const float start[3], const float end[3], float dist,
				int target, float apex[3]);
	void		(*SetYawSpeed)(int e);
	int		(*BuildNearestRoute)(int e, const float threat[3], const float viewofs[3],
				float flMinDist, float flMaxDist);
	int		(*FindCover)(int e, const float threat[3], const float viewofs[3],
				float flMinDist, float flMaxDist);
	int		(*FValidateCover)(int e, const float cover[3]);
	float		(*CoverRadius)(int e);
	int		(*FCanCheckAttacks)(int e);
	void		(*CheckAmmo)(int e);
	int		(*IgnoreConditions)(int e);
	int		(*FValidateHintType)(int e, int hint);
	int		(*FCanActiveIdle)(int e);
	int		(*ISoundMask)(int e);
	CSound		*(*PBestSound)(int e);
	CSound		*(*PBestScent)(int e);
	float		(*HearingSensitivity)(int e);
	void		(*BarnacleVictimBitten)(int e, int barnacle);
	void		(*BarnacleVictimReleased)(int e);
	void		(*PrescheduleThink)(int e);
	int		(*GetDeathActivity)(int e);
	void		(*GibMonster)(int e);
	int		(*HasHumanGibs)(int e);
	int		(*HasAlienGibs)(int e);
	void		(*FadeMonster)(int e);
	int		(*ShouldFadeOnDeath)(int e);
	void		(*BodyTarget)(int e, const float src[3], float out[3]);
	void		(*GetGunPosition)(int e, float out[3]);
	void		(*DeathSound)(int e);
	void		(*AlertSound)(int e);
	void		(*IdleSound)(int e);
	void		(*PainSound)(int e);
	void		(*StopFollowing)(int e, int clearSchedule);
	float		(*ChangeYaw)(int e, int speed);
	/* CSquadMonster / CTalkMonster virtuals */
	void		(*DeclineFollowing)(int e);
	void		(*SetAnswerQuestion)(int e, int speaker);
	int		(*FriendNumber)(int e, int arrayNumber);
	void		(*TalkInit)(int e);
	/* class traits */
	uint8_t		squad;		/* CSquadMonster */
	uint8_t		talk;		/* CTalkMonster */
} mon_vt_t;

/* The monster vtable of entity e (its class's game_class_t.ext); a zeroed
 * table for non-monsters. */
const mon_vt_t	*mon_vt(int e);
/* 1 when e's class is a CBaseMonster (MyMonsterPointer != NULL). */
int	mon_is_monster(int e);

/* ---- CBaseMonster methods (monsters.c, schedule.c, defaultai.c) ----------------- */

/* virtual dispatch (vt slot or the base implementation) */
void	Mon_Look(int e, int iDistance);
void	Mon_RunAI(int e);
void	Mon_MonsterInit(int e);
void	Mon_MonsterInitDead(int e);
void	Mon_BecomeDead(int e);
void	Mon_StartMonster(int e);
int	Mon_BestVisibleEnemy(int e);
int	Mon_FInViewCone(int e, int other);
int	Mon_FInViewConeAt(int e, const float org[3]);
void	Mon_HandleAnimEvent(int e, const MonsterEvent_t *ev);
int	Mon_CheckLocalMove(int e, const float start[3], const float end[3], int target, float *pflDist);
void	Mon_Move(int e, float flInterval);
void	Mon_MoveExecute(int e, int target, const float dir[3], float flInterval);
int	Mon_ShouldAdvanceRoute(int e, float flWaypointDist);
int	Mon_GetStoppedActivity(int e);
void	Mon_Stop(int e);
int	Mon_CheckRangeAttack1(int e, float flDot, float flDist);
int	Mon_CheckRangeAttack2(int e, float flDot, float flDist);
int	Mon_CheckMeleeAttack1(int e, float flDot, float flDist);
int	Mon_CheckMeleeAttack2(int e, float flDot, float flDist);
void	Mon_StartTask(int e, const Task_t *t);
void	Mon_RunTask(int e, const Task_t *t);
const Schedule_t *Mon_GetScheduleOfType(int e, int type);
const Schedule_t *Mon_GetSchedule(int e);
void	Mon_ScheduleChange(int e);
int	Mon_CanPlaySequence(int e, int fDisregardState, int interruptLevel);
int	Mon_CanPlaySentence(int e, int fDisregardState);
/* sentence: a SENTENCE res ("!NAME") or a GROUP res (random pick) */
void	Mon_PlaySentence(int e, res_t sentence, float duration, float volume, float attn);
void	Mon_PlayScriptedSentence(int e, res_t sentence, float duration, float volume, float attn,
		int bConcurrent, int listener);
int	Mon_IsAllowedToSpeak(int e);
int	Mon_GetIdealState(int e);
void	Mon_SetActivity(int e, int act);
int	Mon_CheckEnemy(int e, int enemy);
int	Mon_FTriangulate(int e, const float start[3], const float end[3], float dist, int target,
		float apex[3]);
void	Mon_SetYawSpeed(int e);
int	Mon_BuildNearestRoute(int e, const float threat[3], const float viewofs[3], float mn, float mx);
int	Mon_FindCover(int e, const float threat[3], const float viewofs[3], float mn, float mx);
int	Mon_FValidateCover(int e, const float cover[3]);
float	Mon_CoverRadius(int e);
int	Mon_FCanCheckAttacks(int e);
void	Mon_CheckAmmo(int e);
int	Mon_IgnoreConditions(int e);
int	Mon_FValidateHintType(int e, int hint);
int	Mon_FCanActiveIdle(int e);
int	Mon_ISoundMask(int e);
CSound	*Mon_PBestSound(int e);
CSound	*Mon_PBestScent(int e);
float	Mon_HearingSensitivity(int e);
void	Mon_BarnacleVictimBitten(int e, int barnacle);
void	Mon_BarnacleVictimReleased(int e);
void	Mon_PrescheduleThink(int e);
int	Mon_GetDeathActivity(int e);
void	Mon_GibMonster(int e);
int	Mon_HasHumanGibs(int e);
int	Mon_HasAlienGibs(int e);
void	Mon_FadeMonster(int e);
int	Mon_ShouldFadeOnDeath(int e);
void	Mon_BodyTarget(int e, const float src[3], float out[3]);
void	Mon_GetGunPosition(int e, float out[3]);
void	Mon_DeathSound(int e);
void	Mon_AlertSound(int e);
void	Mon_IdleSound(int e);
void	Mon_PainSound(int e);
void	Mon_StopFollowing(int e, int clearSchedule);
float	Mon_ChangeYaw(int e, int speed);

/* the base implementations (derived classes call them as Base::X) */
void	CBaseMonster_Look(int e, int iDistance);
void	CBaseMonster_RunAI(int e);
void	CBaseMonster_MonsterInit(int e);
void	CBaseMonster_MonsterInitDead(int e);
void	CBaseMonster_BecomeDead(int e);
void	CBaseMonster_StartMonster(int e);
int	CBaseMonster_BestVisibleEnemy(int e);
void	CBaseMonster_HandleAnimEvent(int e, const MonsterEvent_t *ev);
int	CBaseMonster_CheckLocalMove(int e, const float start[3], const float end[3], int target,
		float *pflDist);
void	CBaseMonster_Move(int e, float flInterval);
void	CBaseMonster_MoveExecute(int e, int target, const float dir[3], float flInterval);
int	CBaseMonster_CheckRangeAttack1(int e, float flDot, float flDist);
int	CBaseMonster_CheckRangeAttack2(int e, float flDot, float flDist);
int	CBaseMonster_CheckMeleeAttack1(int e, float flDot, float flDist);
int	CBaseMonster_CheckMeleeAttack2(int e, float flDot, float flDist);
void	CBaseMonster_StartTask(int e, const Task_t *t);
void	CBaseMonster_RunTask(int e, const Task_t *t);
const Schedule_t *CBaseMonster_GetScheduleOfType(int e, int type);
const Schedule_t *CBaseMonster_GetSchedule(int e);
int	CBaseMonster_CanPlaySequence(int e, int fDisregardState, int interruptLevel);
int	CBaseMonster_GetIdealState(int e);
void	CBaseMonster_SetActivity(int e, int act);
int	CBaseMonster_CheckEnemy(int e, int enemy);
int	CBaseMonster_FindCover(int e, const float threat[3], const float viewofs[3], float mn, float mx);
int	CBaseMonster_BuildNearestRoute(int e, const float threat[3], const float viewofs[3], float mn,
		float mx);
int	CBaseMonster_IgnoreConditions(int e);
void	CBaseMonster_BarnacleVictimBitten(int e, int barnacle);
void	CBaseMonster_BarnacleVictimReleased(int e);
int	CBaseMonster_GetDeathActivity(int e);
void	CBaseMonster_GibMonster(int e);
void	CBaseMonster_FadeMonster(int e);
float	CBaseMonster_ChangeYaw(int e, int speed);
int	CBaseMonster_FTriangulate(int e, const float start[3], const float end[3], float dist,
		int target, float apex[3]);

/* non-virtual members */
static inline void Mon_SetConditions(CBaseMonster *m, int c) { m->m_afConditions |= c; }
static inline void Mon_ClearConditions(CBaseMonster *m, int c) { m->m_afConditions &= ~c; }
static inline int Mon_HasConditions(const CBaseMonster *m, int c) { return (m->m_afConditions & c) != 0; }
static inline int Mon_HasAllConditions(const CBaseMonster *m, int c)
{
	return (m->m_afConditions & c) == c;
}
static inline void Mon_Remember(CBaseMonster *m, int b) { m->m_afMemory |= b; }
static inline void Mon_Forget(CBaseMonster *m, int b) { m->m_afMemory &= ~b; }
static inline int Mon_HasMemory(const CBaseMonster *m, int b) { return (m->m_afMemory & b) != 0; }
static inline void Mon_TaskComplete(CBaseMonster *m)
{
	if (!Mon_HasConditions(m, bits_COND_TASK_FAILED))
		m->m_iTaskStatus = TASKSTATUS_COMPLETE;
}
static inline void Mon_TaskFail(CBaseMonster *m) { Mon_SetConditions(m, bits_COND_TASK_FAILED); }
static inline void Mon_TaskBegin(CBaseMonster *m) { m->m_iTaskStatus = TASKSTATUS_RUNNING; }
static inline int Mon_TaskIsComplete(const CBaseMonster *m) { return m->m_iTaskStatus == TASKSTATUS_COMPLETE; }
static inline int Mon_MovementIsComplete(const CBaseMonster *m) { return m->m_movementGoal == MOVEGOAL_NONE; }
static inline int Mon_IsMoving(const CBaseMonster *m) { return m->m_movementGoal != MOVEGOAL_NONE; }
int	Mon_TaskIsRunning(int e);

int	Mon_FHaveSchedule(int e);
int	Mon_FScheduleValid(int e);
void	Mon_ClearSchedule(int e);
int	Mon_FScheduleDone(int e);
void	Mon_ChangeSchedule(int e, const Schedule_t *s);
void	Mon_NextScheduledTask(int e);
void	Mon_MaintainSchedule(int e);
const Task_t *Mon_GetTask(int e);
int	Mon_IScheduleFlags(int e);
void	Mon_SetState(int e, int state);
void	Mon_MovementComplete(int e);
void	Mon_Listen(int e);
float	Mon_FLSoundVolume(int e, const CSound *s);
void	Mon_Eat(int e, float flFullDuration);
int	Mon_FShouldEat(int e);
float	Mon_VecToYaw(int e, const float dir[3]);
float	Mon_FlYawDiff(int e);
float	Mon_DamageForce(int e, float damage);
void	Mon_MakeIdealYaw(int e, const float target[3]);
int	Mon_FacingIdeal(int e);
void	Mon_SetTurnActivity(int e);
void	Mon_SetSequenceByName(int e, const char *name);
void	Mon_SetEyePosition(int e);
void	Mon_CheckAttacks(int e, int target, float flDist);
void	Mon_PushEnemy(int e, int enemy, const float lkp[3]);
int	Mon_PopEnemy(int e);
int	Mon_GetEnemy(int e);
int	Mon_FGetNodeRoute(int e, const float dest[3]);
int	Mon_FRefreshRoute(int e);
int	Mon_FRouteClear(int e);
void	Mon_RouteClear(int e);
void	Mon_RouteNew(int e);
void	Mon_RouteSimplify(int e, int target);
void	Mon_AdvanceRoute(int e, float distance);
int	Mon_BuildRoute(int e, const float goal[3], int iMoveFlag, int target);
int	Mon_RouteClassify(int e, int iMoveFlag);
void	Mon_InsertWaypoint(int e, const float loc[3], int afMoveFlags);
int	Mon_FindLateralCover(int e, const float threat[3], const float viewofs[3]);
int	Mon_FindHintNode(int e);
int	Mon_MoveToNode(int e, int act, float waitTime, const float goal[3]);
int	Mon_MoveToTarget(int e, int act, float waitTime);
int	Mon_MoveToLocation(int e, int act, float waitTime, const float goal[3]);
int	Mon_MoveToEnemy(int e, int act, float waitTime);
float	Mon_OpenDoorAndWait(int e, int door);
int	Mon_FBecomeProne(int e);
int	Mon_CheckTraceHullAttack(int e, float flDist, int iDamage, int iDmgType);
int	Mon_FCheckAITrigger(int e);
int	Mon_BBoxFlat(int e);
int	Mon_ExitScriptedSequence(int e);
int	Mon_CineCleanup(int e);
void	Mon_ShootAtEnemy(int e, const float shootOrigin[3], float out[3]);
int	Mon_DropItem(int e, cls_t cls, const float pos[3], const float ang[3]);
/* the looker's IRelationship (class vt, else the CBaseMonster table) */
int	Mon_IRelationshipOf(int e, int other);
/* the m_pLink list of e's last Look; count (0 when another monster looked
 * since) */
int	Mon_SightList(int e, const uint16_t **list);
void	Mon_SetSequenceIndex(int e, int seq);
/* CBaseMonster::KeyValue (+ CBaseToggle chain) for a monster spawn */
void	CBaseMonster_KeyValue(int e, gid_t16 gid);
/* monstermaker: spawnflags the next spawned monster adds (DispatchSpawn after
 * the maker set them) */
extern uint32_t	mon_spawn_extra_flags;
void	Monstermaker_DeathNotice(gid_t16 maker, int child);
void	Graph_SlotChanged(int slot);

/* think / use functions (funcs.def) */
void	CBaseMonster_MonsterThink(int e);
void	CBaseMonster_CallMonsterThink(int e);
void	CBaseMonster_MonsterInitThink(int e);
void	CBaseMonster_CorpseFallThink(int e);
void	CBaseMonster_MonsterUse(int e, int activator, int caller, int useType, float value);
void	CBaseMonster_CorpseUse(int e, int activator, int caller, int useType, float value);

/* animation helpers over game/studio/anim.h */
int	Mon_LookupActivity(int e, int act);
int	Mon_LookupActivityHeaviest(int e, int act);
int	Mon_LookupSequence(int e, const char *name);
void	Mon_ResetSequenceInfo(int e);
float	Mon_StudioFrameAdvance(int e, float interval);
void	Mon_DispatchAnimEvents(int e, float interval);
void	Mon_SetBoneController(int e, int i, float value);
void	Mon_InitBoneControllers(int e);
void	Mon_SetBodygroup(int e, int group, int value);
int	Mon_GetBodygroup(int e, int group);

/* combat (aicombat.c, HLSDK combat.cpp CBaseMonster parts) */
int	CBaseMonster_TakeDamage(int e, int inflictor, int attacker, float flDamage, int bitsDamageType);
int	CBaseMonster_DeadTakeDamage(int e, int inflictor, int attacker, float flDamage,
		int bitsDamageType);
int	CBaseMonster_TakeHealth(int e, float flHealth, int bitsDamageType);
void	CBaseMonster_Killed(int e, int attacker, int iGib);
void	CBaseMonster_TraceAttack(int e, int attacker, float flDamage, const float dir[3],
		const col_trace_t *tr, int bitsDamageType);
int	CBaseMonster_IRelationship(int e, int other);
int	CBaseMonster_IsAlive(int e);
int	CBaseMonster_BloodColor(int e);
int	CBaseMonster_IsMoving(int e);
/* the game_vt_t slots every monster class fills (CBaseMonster overrides of
 * CBaseEntity virtuals) */
#define MON_GAME_VT_BASE \
	.TakeHealth = CBaseMonster_TakeHealth, \
	.IsAlive = CBaseMonster_IsAlive, \
	.BloodColor = CBaseMonster_BloodColor, \
	.IsMoving = CBaseMonster_IsMoving, \
	.BodyTarget = Mon_BodyTarget
/* ... plus the four a class most often overrides */
#define MON_GAME_VT \
	MON_GAME_VT_BASE, \
	.TraceAttack = CBaseMonster_TraceAttack, \
	.TakeDamage = CBaseMonster_TakeDamage, \
	.Killed = CBaseMonster_Killed, \
	.IRelationship = CBaseMonster_IRelationship
/* common monster spawn prologue: CBaseMonster::KeyValue chain, the
 * monstermaker flags; returns the private block */
CBaseMonster *Mon_SpawnKeys(int e, gid_t16 gid);
/* VEC_HUMAN_HULL_MIN / MAX */
extern const float VEC_HUMAN_HULL_MIN[3], VEC_HUMAN_HULL_MAX[3];
int	Mon_ShouldGibMonster(int e, int iGib);
void	Mon_CallGibMonster(int e);
int	Mon_GetSmallFlinchActivity(int e);
void	Mon_MakeDamageBloodDecal(int e, int cCount, float flNoise, const col_trace_t *tr,
		const float dir[3]);

/* CBaseEntity::FireBullets (monsters; weapons.h constants) */
enum { BULLET_NONE = 0, BULLET_PLAYER_9MM, BULLET_PLAYER_MP5, BULLET_PLAYER_357,
	BULLET_PLAYER_BUCKSHOT, BULLET_PLAYER_CROWBAR, BULLET_MONSTER_9MM, BULLET_MONSTER_MP5,
	BULLET_MONSTER_12MM };
extern const float VECTOR_CONE_1DEGREES[3], VECTOR_CONE_2DEGREES[3], VECTOR_CONE_3DEGREES[3],
	VECTOR_CONE_4DEGREES[3], VECTOR_CONE_5DEGREES[3], VECTOR_CONE_6DEGREES[3],
	VECTOR_CONE_7DEGREES[3], VECTOR_CONE_8DEGREES[3], VECTOR_CONE_9DEGREES[3],
	VECTOR_CONE_10DEGREES[3], VECTOR_CONE_15DEGREES[3], VECTOR_CONE_20DEGREES[3];
void	FireBullets(int e, int cShots, const float src[3], const float dirShooting[3],
		const float spread[3], float flDistance, int iBulletType, int iTracerFreq, int iDamage,
		int attacker);

/* ---- CSquadMonster (aisquad.c) ----------------------------------------------------- */

int	Sqm_SquadRecruit(int e, int searchRadius, int maxMembers);
int	Sqm_SquadCount(int e);
void	Sqm_SquadRemove(int e, int remove);
int	Sqm_SquadAdd(int e, int add);
void	Sqm_SquadMakeEnemy(int e, int enemy);
void	Sqm_SquadPasteEnemyInfo(int e);
void	Sqm_SquadCopyEnemyInfo(int e);
int	Sqm_SquadEnemySplit(int e);
int	Sqm_SquadMemberInRange(int e, const float loc[3], float flDist);
int	Sqm_MySquadLeader(int e);
int	Sqm_MySquadMember(int e, int i);
int	Sqm_InSquad(int e);
int	Sqm_IsLeader(int e);
int	Sqm_OccupySlot(int e, int iDesiredSlot);
void	Sqm_VacateSlot(int e);
int	Sqm_NoFriendlyFire(int e);
void	Sqm_Init(int e);
int	CSquadMonster_CheckEnemy(int e, int enemy);
void	CSquadMonster_StartMonster(int e);
void	CSquadMonster_ScheduleChange(int e);
void	CSquadMonster_Killed(int e, int attacker, int iGib);
int	CSquadMonster_FValidateCover(int e, const float cover[3]);
int	CSquadMonster_GetIdealState(int e);
const Schedule_t *CSquadMonster_GetScheduleOfType(int e, int type);

/* ---- CTalkMonster (talkmonster.c) ---------------------------------------------------- */

extern float	g_talkWaitTime;
void	Tlk_Precache(int e);
int	CTalkMonster_TakeDamage(int e, int inflictor, int attacker, float flDamage, int bits);
void	CTalkMonster_Touch(int e, int other);
void	CTalkMonster_Killed(int e, int attacker, int iGib);
int	CTalkMonster_IRelationship(int e, int other);
int	CTalkMonster_CanPlaySentence(int e, int fDisregardState);
void	CTalkMonster_PlaySentence(int e, res_t sentence, float duration, float volume, float attn);
void	CTalkMonster_PlayScriptedSentence(int e, res_t sentence, float duration, float volume,
		float attn, int bConcurrent, int listener);
void	CTalkMonster_SetActivity(int e, int act);
const Schedule_t *CTalkMonster_GetScheduleOfType(int e, int type);
void	CTalkMonster_StartTask(int e, const Task_t *t);
void	CTalkMonster_RunTask(int e, const Task_t *t);
void	CTalkMonster_HandleAnimEvent(int e, const MonsterEvent_t *ev);
void	CTalkMonster_PrescheduleThink(int e);
void	CTalkMonster_StopFollowing(int e, int clearSchedule);
void	CTalkMonster_SetAnswerQuestion(int e, int speaker);
void	CTalkMonster_FollowerUse(int e, int activator, int caller, int useType, float value);
void	Tlk_TalkInit(int e);
/* CTalkMonster::m_szFriends (shared, rewritten by TalkInit as HLSDK) */
void	Tlk_SetFriends(cls_t a, cls_t b, cls_t c);
int	Tlk_FindNearestFriend(int e, int fPlayer);
float	Tlk_TargetDistance(int e);
int	Tlk_GetVoicePitch(int e);
void	Tlk_IdleRespond(int e);
int	Tlk_FIdleSpeak(int e);
int	Tlk_FIdleStare(int e);
int	Tlk_FIdleHello(int e);
void	Tlk_IdleHeadTurn(int e, const float vecFriend[3]);
int	Tlk_FOkToSpeak(int e);
void	Tlk_TrySmellTalk(int e);
int	Tlk_EnumFriends(int e, int prev, int listNumber, int bTrace);
void	Tlk_AlertFriends(int e);
void	Tlk_ShutUpFriends(int e);
int	Tlk_IsTalking(int e);
void	Tlk_Talk(int e, float flDuration);
int	Tlk_CanFollow(int e);
int	Tlk_IsFollowing(int e);
void	Tlk_StartFollowing(int e, int leader);
void	Tlk_LimitFollowers(int e, int player, int maxFollowers);
void	Tlk_StopTalking(int e);
void	Mon_SentenceStop(int e);
float	UTIL_DotPoints(const float src[3], const float check[3], const float dir[3]);
extern const Schedule_t	slIdleResponse[], slIdleSpeak[], slIdleHello[], slIdleSpeakWait[],
	slIdleStopShooting[], slMoveAway[], slMoveAwayFollow[], slMoveAwayFail[],
	slTlkIdleWatchClient[], slTlkIdleEyecontact[];
int	Tlk_KeyValue(int e, gid_t16 gid);	/* UseSentence / UnUseSentence */

/* ---- sentences (sentences.c; HLSDK util.cpp SENTENCEG_*, sound.cpp) ------------------ */

#define CSENTENCEG_MAX		200
#define CSENTENCE_LRU_MAX	32
/* SENTENCEG_PlayRndSz on a GROUP res_t: picks a random sentence id of the
 * group and plays it on CHAN_VOICE; returns the sentence id (-1 none). */
int	SENTENCEG_PlayRndSz(int e, res_t group, float volume, float attn, int flags, int pitch);
/* SENTENCEG_PlaySequentialSz: next sentence of the group after ipick
 * (wraps unless freset == 0 at the end); returns the next ipick. */
int	SENTENCEG_PlaySequentialSz(int e, res_t group, float volume, float attn, int flags, int pitch,
		int ipick, int freset);
/* USENTENCEG_Pick no-repeat pick (per-group LRU, HLSDK) -> sentence id, -1 none */
int	SENTENCEG_Pick(res_t group);
void	SENTENCEG_Stop(int e, res_t group, int ipick);
void	SENTENCEG_Init(void);
/* Sentence id of a "!NAME" sentence res_t (RES_K_SENTENCE). */
static inline uint16_t sentence_id(res_t r) { return (uint16_t)RES_INDEX(r); }

/* ---- node graph (nodes.c) ------------------------------------------------------------- */

#define NODE_HEIGHT		8
#define bits_NODE_LAND		(1 << 0)
#define bits_NODE_AIR		(1 << 1)
#define bits_NODE_WATER		(1 << 2)
#define bits_NODE_GROUP_REALM	(bits_NODE_LAND | bits_NODE_AIR | bits_NODE_WATER)
#define NODE_SMALL_HULL		0
#define NODE_HUMAN_HULL		1
#define NODE_LARGE_HULL		2
#define NODE_FLY_HULL		3
#define MAX_PATH_SIZE		10
#define NO_NODE			-1
#define HINT_NONE		0

int	Graph_Available(int slot);
int	Graph_NodeCount(int slot);
const float *Graph_NodeOrigin(int slot, int node);
int	Graph_NodeType(int e);
int	Graph_HullIndex(int e);
int	Graph_FindNearestNode(int e, const float org[3], int afNodeTypes);
/* FindShortestPath: path of node indices into piPath (<= MAX_PATH_SIZE);
 * returns its length, 0 when none. */
int	Graph_FindShortestPath(int slot, int *piPath, int iStart, int iDest, int iHull, int afCapMask);
int	Graph_LinkPassable(int e, int slot, int src, int linkidx, int afCapMask);
/* the door to open between two route nodes (HandleLinkEnt), -1 none */
int	Graph_RouteDoor(int e, const float from[3], const float to[3]);
int	Graph_BuildNearestRoute(int e, const float threat[3], const float viewofs[3], float mn,
		float mx);
int	Graph_FindCover(int e, const float threat[3], const float viewofs[3], float mn, float mx,
		float out[3]);

/* ---- scripted (scripted.c) -------------------------------------------------------------- */

void	Script_SequenceDone(int script, int monster);
void	Script_FixScriptMonsterSchedule(int script, int monster);
int	Script_IgnoreConditions(int script);
int	Script_CanInterrupt(int script);
void	Script_AllowInterrupt(int script, int fAllow);
int	Script_FCanOverrideState(int script);
void	Script_CancelScript(int script);
int	Script_StartSequence(int script, int monster, int play, int completeOnEmpty);
/* m_iszPlay / m_iszIdle sequence of the script on the monster's model
 * (resolved once), -1 none. */
int	Script_PlaySequence(int script);
int	Script_IdleSequence(int script);
int	Script_HasIdle(int script);
int	Script_MoveTo(int script);
int	Script_DelayRemove(int script);
void	Script_DelayStart(int script, int state);
int	Script_ScriptedSentenceAcceptable(int e);
/* TASK_WAIT_FOR_SCRIPT: m_iDelay <= 0 and time >= m_startTime */
int	Script_ReadyToStart(int script);
/* m_iszIdle == m_iszPlay */
int	Script_IdleIsPlay(int script);

/* ---- per-frame accounting (PROFILE roll-up "prof mon_us") ------------------------------------ */

typedef struct {
	uint32_t	thinks, routes, local_moves, us, us_max, monsters, scripts;
} mon_stats_t;
void	mon_stats(mon_stats_t *s, int reset);
void	mon_stats_frame(void);
void	mon_stats_route(void);
void	mon_stats_script(void);

/* misc */
#define g_flMeleeRange	64.0f
#define g_flMediumRange	512.0f
#define g_flLongRange	8192.0f

/* HLSDK RANDOM_SOUND_ARRAY / PRECACHE_SOUND_ARRAY over arrays of res.def
 * line ids (R_<IDENT>) */
#define RANDOM_SOUND_ARRAY(a)	res_def((a)[RANDOM_LONG(0, ARRAYSIZE(a) - 1)])
#define PRECACHE_SOUND_ARRAY(a)	do { for (int i_ = 0; i_ < ARRAYSIZE(a); i_++) PRECACHE(res_def((a)[i_])); } while (0)
/* the player's punch angle (pev->punchangle of the player) */
void	Mon_PunchPlayer(int e, float x, float z);
extern const float VEC_VIEW[3];

/* skill-dependent values (HLSDK skill.h / skill.cfg values for the monsters) */
float	mon_skill(const float v[3]);

#endif
