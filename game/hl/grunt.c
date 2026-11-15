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
 * grunt.c -- monster_human_grunt, monster_grunt_repel, monster_hgrunt_dead
 * (HLSDK dlls/hgrunt.cpp CHGrunt, CHGruntRepel, CDeadHGrunt; h_ai.cpp
 * VecCheckToss / VecCheckThrow). pev->weapons is the grunt's m_weapons
 * ("weapons" key). The player's entvars (velocity, waterlevel,
 * punchangle) are its ent_player_t.
 */
#include <math.h>

#include "hl64/fx.h"
#include "hl64/settings.h"
#include "engine/phys/player.h"
#include "game/hl/monster.h"
#include "game/hl/grenade.h"
#include "game/hl/player.h"

#define GRUNT_CLIP_SIZE			36
#define GRUNT_ATTN			ATTN_NORM
#define HGRUNT_LIMP_HEALTH		20
#define HGRUNT_SENTENCE_VOLUME		0.35f

#define HGRUNT_9MMAR			(1 << 0)
#define HGRUNT_HANDGRENADE		(1 << 1)
#define HGRUNT_GRENADELAUNCHER		(1 << 2)
#define HGRUNT_SHOTGUN			(1 << 3)

#define HEAD_GROUP			1
#define HEAD_GRUNT			0
#define HEAD_COMMANDER			1
#define HEAD_SHOTGUN			2
#define HEAD_M203			3
#define GUN_GROUP			2
#define GUN_MP5				0
#define GUN_SHOTGUN			1
#define GUN_NONE			2

#define HGRUNT_AE_RELOAD		2
#define HGRUNT_AE_KICK			3
#define HGRUNT_AE_BURST1		4
#define HGRUNT_AE_BURST2		5
#define HGRUNT_AE_BURST3		6
#define HGRUNT_AE_GREN_TOSS		7
#define HGRUNT_AE_GREN_LAUNCH		8
#define HGRUNT_AE_GREN_DROP		9
#define HGRUNT_AE_CAUGHT_ENEMY		10
#define HGRUNT_AE_DROP_GUN		11

/* const.h TE_BOUNCE_SHELL / TE_BOUNCE_SHOTSHELL */
#define TE_BOUNCE_SHELL			1
#define TE_BOUNCE_SHOTSHELL		2

#define SKILL_HARD			3

enum {
	SCHED_GRUNT_SUPPRESS = LAST_COMMON_SCHEDULE + 1,
	SCHED_GRUNT_ESTABLISH_LINE_OF_FIRE,
	SCHED_GRUNT_COVER_AND_RELOAD,
	SCHED_GRUNT_SWEEP,
	SCHED_GRUNT_FOUND_ENEMY,
	SCHED_GRUNT_REPEL,
	SCHED_GRUNT_REPEL_ATTACK,
	SCHED_GRUNT_REPEL_LAND,
	SCHED_GRUNT_WAIT_FACE_ENEMY,
	SCHED_GRUNT_TAKECOVER_FAILED,
	SCHED_GRUNT_ELOF_FAIL,
};

enum {
	TASK_GRUNT_FACE_TOSS_DIR = LAST_COMMON_TASK + 1,
	TASK_GRUNT_SPEAK_SENTENCE,
	TASK_GRUNT_CHECK_FIRE,
};

#define bits_COND_GRUNT_NOFIRE	bits_COND_SPECIAL1

enum {
	HGRUNT_SENT_NONE = -1,
	HGRUNT_SENT_GREN = 0,
	HGRUNT_SENT_ALERT,
	HGRUNT_SENT_MONSTER,
	HGRUNT_SENT_COVER,
	HGRUNT_SENT_THROW,
	HGRUNT_SENT_CHARGE,
	HGRUNT_SENT_TAUNT,
};

static const float sk_hgruntHealth[3] = { 50, 50, 80 };
static const float sk_hgruntDmgKick[3] = { 5, 10, 10 };
static const float sk_hgruntShotgunPellets[3] = { 3, 5, 6 };
static const float sk_hgruntGrenadeSpeed[3] = { 400, 600, 800 };

typedef struct {
	CBASEENTITY_FIELDS
	CBASEDELAY_FIELDS
	CBASEANIMATING_FIELDS
	CBASETOGGLE_FIELDS
	CBASEMONSTER_FIELDS
	CSQUADMONSTER_FIELDS
	float		m_flNextGrenadeCheck;
	float		m_flNextPainTime;
	float		m_vecTossVelocity[3];
	uint8_t		m_fThrowGrenade;
	uint8_t		m_fStanding;
	uint8_t		m_fFirstEncounter;
	int8_t		m_iSentence;
	int		m_cClipSize;
	int		m_voicePitch;
	int		m_weapons;	/* pev->weapons */
} CHGrunt;

#define HG(e)	((CHGrunt *)ent_priv(e))

/* true if an idle grunt asked a question; cleared when someone answers */
static int g_fGruntQuestion;

/* pGruntSentences[] ("HG_MONSTER" is no group in sentences.txt) */
static const int16_t hgSentences[] = {
	R_GRP_HG_GREN, R_GRP_HG_ALERT, -1, R_GRP_HG_COVER, R_GRP_HG_THROW, R_GRP_HG_CHARGE,
	R_GRP_HG_TAUNT,
};
static const int16_t hgPain[] = { R_HG_PAIN3, R_HG_PAIN4, R_HG_PAIN5, R_HG_PAIN1, R_HG_PAIN2 };
static const int16_t hgDie[] = { R_HG_DIE1, R_HG_DIE2, R_HG_DIE3 };

/* ---- schedules --------------------------------------------------------------------- */

#define T(t, d)	{ t, (float)(d) }

static const Task_t tlGruntFail[] = {
	T(TASK_STOP_MOVING, 0), T(TASK_SET_ACTIVITY, ACT_IDLE), T(TASK_WAIT, 2), T(TASK_WAIT_PVS, 0),
};
static const Schedule_t slGruntFail[] = {
	SCHED(tlGruntFail, bits_COND_CAN_RANGE_ATTACK1 | bits_COND_CAN_RANGE_ATTACK2 |
		bits_COND_CAN_MELEE_ATTACK1 | bits_COND_CAN_MELEE_ATTACK2, 0, "Grunt Fail")
};

static const Task_t tlGruntCombatFail[] = {
	T(TASK_STOP_MOVING, 0), T(TASK_SET_ACTIVITY, ACT_IDLE), T(TASK_WAIT_FACE_ENEMY, 2),
	T(TASK_WAIT_PVS, 0),
};
static const Schedule_t slGruntCombatFail[] = {
	SCHED(tlGruntCombatFail, bits_COND_CAN_RANGE_ATTACK1 | bits_COND_CAN_RANGE_ATTACK2, 0,
		"Grunt Combat Fail")
};

static const Task_t tlGruntVictoryDance[] = {
	T(TASK_STOP_MOVING, 0), T(TASK_FACE_ENEMY, 0), T(TASK_WAIT, 1.5),
	T(TASK_GET_PATH_TO_ENEMY_CORPSE, 0), T(TASK_WALK_PATH, 0), T(TASK_WAIT_FOR_MOVEMENT, 0),
	T(TASK_FACE_ENEMY, 0), T(TASK_PLAY_SEQUENCE, ACT_VICTORY_DANCE),
};
static const Schedule_t slGruntVictoryDance[] = {
	SCHED(tlGruntVictoryDance, bits_COND_NEW_ENEMY | bits_COND_LIGHT_DAMAGE |
		bits_COND_HEAVY_DAMAGE, 0, "GruntVictoryDance")
};

static const Task_t tlGruntEstablishLineOfFire[] = {
	T(TASK_SET_FAIL_SCHEDULE, SCHED_GRUNT_ELOF_FAIL), T(TASK_GET_PATH_TO_ENEMY, 0),
	T(TASK_GRUNT_SPEAK_SENTENCE, 0), T(TASK_RUN_PATH, 0), T(TASK_WAIT_FOR_MOVEMENT, 0),
};
static const Schedule_t slGruntEstablishLineOfFire[] = {
	SCHED(tlGruntEstablishLineOfFire, bits_COND_NEW_ENEMY | bits_COND_ENEMY_DEAD |
		bits_COND_CAN_RANGE_ATTACK1 | bits_COND_CAN_MELEE_ATTACK1 |
		bits_COND_CAN_RANGE_ATTACK2 | bits_COND_CAN_MELEE_ATTACK2 | bits_COND_HEAR_SOUND,
		bits_SOUND_DANGER, "GruntEstablishLineOfFire")
};

static const Task_t tlGruntFoundEnemy[] = {
	T(TASK_STOP_MOVING, 0), T(TASK_FACE_ENEMY, 0),
	T(TASK_PLAY_SEQUENCE_FACE_ENEMY, ACT_SIGNAL1),
};
static const Schedule_t slGruntFoundEnemy[] = {
	SCHED(tlGruntFoundEnemy, bits_COND_HEAR_SOUND, bits_SOUND_DANGER, "GruntFoundEnemy")
};

static const Task_t tlGruntCombatFace1[] = {
	T(TASK_STOP_MOVING, 0), T(TASK_SET_ACTIVITY, ACT_IDLE), T(TASK_FACE_ENEMY, 0),
	T(TASK_WAIT, 1.5), T(TASK_SET_SCHEDULE, SCHED_GRUNT_SWEEP),
};
static const Schedule_t slGruntCombatFace[] = {
	SCHED(tlGruntCombatFace1, bits_COND_NEW_ENEMY | bits_COND_ENEMY_DEAD |
		bits_COND_CAN_RANGE_ATTACK1 | bits_COND_CAN_RANGE_ATTACK2, 0, "Combat Face")
};

#define FIRE_ROUND	T(TASK_FACE_ENEMY, 0), T(TASK_GRUNT_CHECK_FIRE, 0), T(TASK_RANGE_ATTACK1, 0)

static const Task_t tlGruntSignalSuppress[] = {
	T(TASK_STOP_MOVING, 0), T(TASK_FACE_IDEAL, 0),
	T(TASK_PLAY_SEQUENCE_FACE_ENEMY, ACT_SIGNAL2),
	FIRE_ROUND, FIRE_ROUND, FIRE_ROUND, FIRE_ROUND, FIRE_ROUND,
};
static const Schedule_t slGruntSignalSuppress[] = {
	SCHED(tlGruntSignalSuppress, bits_COND_ENEMY_DEAD | bits_COND_LIGHT_DAMAGE |
		bits_COND_HEAVY_DAMAGE | bits_COND_HEAR_SOUND | bits_COND_GRUNT_NOFIRE |
		bits_COND_NO_AMMO_LOADED, bits_SOUND_DANGER, "SignalSuppress")
};

static const Task_t tlGruntSuppress[] = {
	T(TASK_STOP_MOVING, 0), FIRE_ROUND, FIRE_ROUND, FIRE_ROUND, FIRE_ROUND, FIRE_ROUND,
};
static const Schedule_t slGruntSuppress[] = {
	SCHED(tlGruntSuppress, bits_COND_ENEMY_DEAD | bits_COND_LIGHT_DAMAGE |
		bits_COND_HEAVY_DAMAGE | bits_COND_HEAR_SOUND | bits_COND_GRUNT_NOFIRE |
		bits_COND_NO_AMMO_LOADED, bits_SOUND_DANGER, "Suppress")
};

static const Task_t tlGruntWaitInCover[] = {
	T(TASK_STOP_MOVING, 0), T(TASK_SET_ACTIVITY, ACT_IDLE), T(TASK_WAIT_FACE_ENEMY, 1),
};
static const Schedule_t slGruntWaitInCover[] = {
	SCHED(tlGruntWaitInCover, bits_COND_NEW_ENEMY | bits_COND_HEAR_SOUND |
		bits_COND_CAN_RANGE_ATTACK1 | bits_COND_CAN_RANGE_ATTACK2 |
		bits_COND_CAN_MELEE_ATTACK1 | bits_COND_CAN_MELEE_ATTACK2, bits_SOUND_DANGER,
		"GruntWaitInCover")
};

static const Task_t tlGruntTakeCover1[] = {
	T(TASK_STOP_MOVING, 0), T(TASK_SET_FAIL_SCHEDULE, SCHED_GRUNT_TAKECOVER_FAILED),
	T(TASK_WAIT, 0.2), T(TASK_FIND_COVER_FROM_ENEMY, 0), T(TASK_GRUNT_SPEAK_SENTENCE, 0),
	T(TASK_RUN_PATH, 0), T(TASK_WAIT_FOR_MOVEMENT, 0), T(TASK_REMEMBER, bits_MEMORY_INCOVER),
	T(TASK_SET_SCHEDULE, SCHED_GRUNT_WAIT_FACE_ENEMY),
};
static const Schedule_t slGruntTakeCover[] = { SCHED(tlGruntTakeCover1, 0, 0, "TakeCover") };

static const Task_t tlGruntGrenadeCover1[] = {
	T(TASK_STOP_MOVING, 0), T(TASK_FIND_COVER_FROM_ENEMY, 99),
	T(TASK_FIND_FAR_NODE_COVER_FROM_ENEMY, 384), T(TASK_PLAY_SEQUENCE, ACT_SPECIAL_ATTACK1),
	T(TASK_CLEAR_MOVE_WAIT, 0), T(TASK_RUN_PATH, 0), T(TASK_WAIT_FOR_MOVEMENT, 0),
	T(TASK_SET_SCHEDULE, SCHED_GRUNT_WAIT_FACE_ENEMY),
};
static const Schedule_t slGruntGrenadeCover[] = { SCHED(tlGruntGrenadeCover1, 0, 0, "GrenadeCover") };

static const Task_t tlGruntTossGrenadeCover1[] = {
	T(TASK_FACE_ENEMY, 0), T(TASK_RANGE_ATTACK2, 0),
	T(TASK_SET_SCHEDULE, SCHED_TAKE_COVER_FROM_ENEMY),
};
static const Schedule_t slGruntTossGrenadeCover[] = {
	SCHED(tlGruntTossGrenadeCover1, 0, 0, "TossGrenadeCover")
};

static const Task_t tlGruntTakeCoverFromBestSound[] = {
	T(TASK_SET_FAIL_SCHEDULE, SCHED_COWER), T(TASK_STOP_MOVING, 0),
	T(TASK_FIND_COVER_FROM_BEST_SOUND, 0), T(TASK_RUN_PATH, 0), T(TASK_WAIT_FOR_MOVEMENT, 0),
	T(TASK_REMEMBER, bits_MEMORY_INCOVER), T(TASK_TURN_LEFT, 179),
};
static const Schedule_t slGruntTakeCoverFromBestSound[] = {
	SCHED(tlGruntTakeCoverFromBestSound, 0, 0, "GruntTakeCoverFromBestSound")
};

static const Task_t tlGruntHideReload[] = {
	T(TASK_STOP_MOVING, 0), T(TASK_SET_FAIL_SCHEDULE, SCHED_RELOAD),
	T(TASK_FIND_COVER_FROM_ENEMY, 0), T(TASK_RUN_PATH, 0), T(TASK_WAIT_FOR_MOVEMENT, 0),
	T(TASK_REMEMBER, bits_MEMORY_INCOVER), T(TASK_FACE_ENEMY, 0),
	T(TASK_PLAY_SEQUENCE, ACT_RELOAD),
};
static const Schedule_t slGruntHideReload[] = {
	SCHED(tlGruntHideReload, bits_COND_HEAVY_DAMAGE | bits_COND_HEAR_SOUND, bits_SOUND_DANGER,
		"GruntHideReload")
};

static const Task_t tlGruntSweep[] = {
	T(TASK_TURN_LEFT, 179), T(TASK_WAIT, 1), T(TASK_TURN_LEFT, 179), T(TASK_WAIT, 1),
};
static const Schedule_t slGruntSweep[] = {
	SCHED(tlGruntSweep, bits_COND_NEW_ENEMY | bits_COND_LIGHT_DAMAGE | bits_COND_HEAVY_DAMAGE |
		bits_COND_CAN_RANGE_ATTACK1 | bits_COND_CAN_RANGE_ATTACK2 | bits_COND_HEAR_SOUND,
		bits_SOUND_WORLD | bits_SOUND_DANGER | bits_SOUND_PLAYER, "Grunt Sweep")
};

static const Task_t tlGruntRangeAttack1A[] = {
	T(TASK_STOP_MOVING, 0), T(TASK_PLAY_SEQUENCE_FACE_ENEMY, ACT_CROUCH),
	T(TASK_GRUNT_CHECK_FIRE, 0), T(TASK_RANGE_ATTACK1, 0), FIRE_ROUND, FIRE_ROUND, FIRE_ROUND,
};
static const Schedule_t slGruntRangeAttack1A[] = {
	SCHED(tlGruntRangeAttack1A, bits_COND_NEW_ENEMY | bits_COND_ENEMY_DEAD |
		bits_COND_HEAVY_DAMAGE | bits_COND_ENEMY_OCCLUDED | bits_COND_HEAR_SOUND |
		bits_COND_GRUNT_NOFIRE | bits_COND_NO_AMMO_LOADED, bits_SOUND_DANGER, "Range Attack1A")
};

static const Task_t tlGruntRangeAttack1B[] = {
	T(TASK_STOP_MOVING, 0), T(TASK_PLAY_SEQUENCE_FACE_ENEMY, ACT_IDLE_ANGRY),
	T(TASK_GRUNT_CHECK_FIRE, 0), T(TASK_RANGE_ATTACK1, 0), FIRE_ROUND, FIRE_ROUND, FIRE_ROUND,
};
static const Schedule_t slGruntRangeAttack1B[] = {
	SCHED(tlGruntRangeAttack1B, bits_COND_NEW_ENEMY | bits_COND_ENEMY_DEAD |
		bits_COND_HEAVY_DAMAGE | bits_COND_ENEMY_OCCLUDED | bits_COND_NO_AMMO_LOADED |
		bits_COND_GRUNT_NOFIRE | bits_COND_HEAR_SOUND, bits_SOUND_DANGER, "Range Attack1B")
};

static const Task_t tlGruntRangeAttack2[] = {
	T(TASK_STOP_MOVING, 0), T(TASK_GRUNT_FACE_TOSS_DIR, 0),
	T(TASK_PLAY_SEQUENCE, ACT_RANGE_ATTACK2), T(TASK_SET_SCHEDULE, SCHED_GRUNT_WAIT_FACE_ENEMY),
};
static const Schedule_t slGruntRangeAttack2[] = { SCHED(tlGruntRangeAttack2, 0, 0, "RangeAttack2") };

static const Task_t tlGruntRepel[] = {
	T(TASK_STOP_MOVING, 0), T(TASK_FACE_IDEAL, 0), T(TASK_PLAY_SEQUENCE, ACT_GLIDE),
};
static const Schedule_t slGruntRepel[] = {
	SCHED(tlGruntRepel, bits_COND_SEE_ENEMY | bits_COND_NEW_ENEMY | bits_COND_LIGHT_DAMAGE |
		bits_COND_HEAVY_DAMAGE | bits_COND_HEAR_SOUND,
		bits_SOUND_DANGER | bits_SOUND_COMBAT | bits_SOUND_PLAYER, "Repel")
};

static const Task_t tlGruntRepelAttack[] = {
	T(TASK_STOP_MOVING, 0), T(TASK_FACE_ENEMY, 0), T(TASK_PLAY_SEQUENCE, ACT_FLY),
};
static const Schedule_t slGruntRepelAttack[] = {
	SCHED(tlGruntRepelAttack, bits_COND_ENEMY_OCCLUDED, 0, "Repel Attack")
};

static const Task_t tlGruntRepelLand[] = {
	T(TASK_STOP_MOVING, 0), T(TASK_PLAY_SEQUENCE, ACT_LAND),
	T(TASK_GET_PATH_TO_LASTPOSITION, 0), T(TASK_RUN_PATH, 0), T(TASK_WAIT_FOR_MOVEMENT, 0),
	T(TASK_CLEAR_LASTPOSITION, 0),
};
static const Schedule_t slGruntRepelLand[] = {
	SCHED(tlGruntRepelLand, bits_COND_SEE_ENEMY | bits_COND_NEW_ENEMY | bits_COND_LIGHT_DAMAGE |
		bits_COND_HEAVY_DAMAGE | bits_COND_HEAR_SOUND,
		bits_SOUND_DANGER | bits_SOUND_COMBAT | bits_SOUND_PLAYER, "Repel Land")
};

/* ---- entvars of a client or an entity ------------------------------------------------ */

static const float *velocity_of(int e)
{
	return IsPlayer(e) ? phy_player()->velocity : ent_mot(e)->velocity;
}

static int waterlevel_of(int e)
{
	return IsPlayer(e) ? phy_player()->waterlevel : ent_phys(e)->waterlevel;
}

/* ---- h_ai.cpp ------------------------------------------------------------------------ */

/* VecCheckToss: velocity to lob from spot1 to land near spot2; 0 if not feasible */
static int VecCheckToss(int e, const float spot1[3], const float spot2in[3], float flGravityAdj,
	float out[3])
{
	col_trace_t	tr;
	float		spot2[3], mid[3], up[3], apex[3], vel[3], r;
	float		flGravity = settings.gravity * flGravityAdj, d1, d2, t1, t2;

	VecSet(out, 0, 0, 0);
	if (spot2in[2] - spot1[2] > 500)
		return 0;	/* too high */
	UTIL_MakeVectors(ent_cv(e)->angles);
	/* a little to the left or right, not right down on the enemy's bean */
	/* draws in source order (operand order is unspecified, K10) */
	r = RANDOM_FLOAT(-8, 8);
	r += RANDOM_FLOAT(-16, 16);
	VecMA(spot2in, r, gpGlobals.v_right, spot2);
	r = RANDOM_FLOAT(-8, 8);
	r += RANDOM_FLOAT(-16, 16);
	VecMA(spot2, r, gpGlobals.v_forward, spot2);
	/* rough idea of how high it can be thrown */
	VecSub(spot2, spot1, vel);
	VecMA(spot1, 0.5f, vel, mid);
	VecCopy(mid, up);
	up[2] += 500;
	UTIL_TraceLine(mid, up, ignore_monsters, e, &tr);
	VecCopy(tr.end, mid);
	mid[2] -= 15;	/* so it doesn't hit the ceiling */
	if (mid[2] < spot1[2] || mid[2] < spot2[2])
		return 0;	/* not enough space */
	d1 = mid[2] - spot1[2];
	d2 = mid[2] - spot2[2];
	t1 = sqrtf(d1 / (0.5f * flGravity));
	t2 = sqrtf(d2 / (0.5f * flGravity));
	if (t1 < 0.1f)
		return 0;	/* too close */
	VecSub(spot2, spot1, vel);
	VecScale(vel, 1.0f / (t1 + t2), vel);
	vel[2] = flGravity * t1;
	VecMA(spot1, t1, vel, apex);
	apex[2] = mid[2];
	UTIL_TraceLine(spot1, apex, dont_ignore_monsters, e, &tr);
	if (tr.frac != 1.0f)
		return 0;
	UTIL_TraceLine(spot2, apex, ignore_monsters, e, &tr);
	if (tr.frac != 1.0f)
		return 0;
	VecCopy(vel, out);
	return 1;
}

/* VecCheckThrow: velocity to throw from spot1 to hit spot2; 0 if not feasible */
static int VecCheckThrow(int e, const float spot1[3], const float spot2[3], float flSpeed,
	float flGravityAdj, float out[3])
{
	col_trace_t	tr;
	float		flGravity = settings.gravity * flGravityAdj, vel[3], apex[3], d[3], t;

	VecSet(out, 0, 0, 0);
	VecSub(spot2, spot1, vel);
	t = VecLen(vel) / flSpeed;	/* throw at a constant time */
	VecScale(vel, 1.0f / t, vel);
	vel[2] += flGravity * t * 0.5f;	/* compensate for gravity loss */
	VecSub(spot2, spot1, d);
	VecMA(spot1, 0.5f, d, apex);
	apex[2] += 0.5f * flGravity * (t * 0.5f) * (t * 0.5f);
	UTIL_TraceLine(spot1, apex, dont_ignore_monsters, e, &tr);
	if (tr.frac != 1.0f)
		return 0;
	UTIL_TraceLine(spot2, apex, ignore_monsters, e, &tr);
	if (tr.frac != 1.0f)
		return 0;
	VecCopy(vel, out);
	return 1;
}

/* ---- CHGrunt ------------------------------------------------------------------------- */

static int Hg_FOkToSpeak(int e)
{
	if (gtime() <= g_talkWaitTime)
		return 0;	/* someone else is talking */
	if ((CBase(e)->spawnflags & SF_MONSTER_GAG) && HG(e)->m_MonsterState != MONSTERSTATE_COMBAT)
		return 0;	/* no talking outside of combat if gagged */
	return 1;
}

static void Hg_JustSpoke(int e)
{
	g_talkWaitTime = gtime() + RANDOM_FLOAT(1.5f, 2.0f);
	HG(e)->m_iSentence = HGRUNT_SENT_NONE;
}

static void Hg_Say(int e, res_t group)
{
	SENTENCEG_PlayRndSz(e, group, HGRUNT_SENTENCE_VOLUME, GRUNT_ATTN, 0, HG(e)->m_voicePitch);
}

/* SpeakSentence: the cued-up sentence, once the action is known to be possible */
static void Hg_SpeakSentence(int e)
{
	CHGrunt *g = HG(e);

	if (g->m_iSentence == HGRUNT_SENT_NONE)
		return;
	if (Hg_FOkToSpeak(e))
	{
		if (hgSentences[g->m_iSentence] >= 0)
			Hg_Say(e, res_def(hgSentences[g->m_iSentence]));
		Hg_JustSpoke(e);
	}
}

/* Alien grunts and gargantuas are the human grunt's nemesis */
static int Hg_IRelationship(int e, int other)
{
	if (ent_idc(other)->cls == CLS_MONSTER_GARGANTUA)
		return R_NM;
	return CBaseMonster_IRelationship(e, other);
}

static void throw_item(int it)
{
	float v[3];

	if (it < 0)
		return;
	/* draws in source order (argument order is unspecified, K10) */
	v[0] = RANDOM_FLOAT(-100, 100);
	v[1] = RANDOM_FLOAT(-100, 100);
	v[2] = RANDOM_FLOAT(200, 300);
	ent_set_velocity(it, v);
	VecSet(v, 0, RANDOM_FLOAT(200, 400), 0);
	ent_set_avelocity(it, v);
}

/* GibMonster: make the gun fly through the air */
static void Hg_GibMonster(int e)
{
	CHGrunt	*g = HG(e);
	float	pos[3], ang[3] = { 0, 0, 0 };

	if (Mon_GetBodygroup(e, GUN_GROUP) != GUN_NONE)
	{
		gs_attachment(e, 0, pos);
		throw_item(Mon_DropItem(e, (g->m_weapons & HGRUNT_SHOTGUN) ? CLS_WEAPON_SHOTGUN :
			CLS_WEAPON_9MMAR, pos, ang));
		if (g->m_weapons & HGRUNT_GRENADELAUNCHER)
			throw_item(Mon_DropItem(e, CLS_AMMO_ARGRENADES, pos, ang));
	}
	CBaseMonster_GibMonster(e);
}

/* grunts hear the DANGER sound of hand grenades */
static int Hg_ISoundMask(int e)
{
	(void)e;
	return bits_SOUND_WORLD | bits_SOUND_COMBAT | bits_SOUND_PLAYER | bits_SOUND_DANGER;
}

static void Hg_PrescheduleThink(int e)
{
	CHGrunt	*g = HG(e);
	int	leader;

	if (!Sqm_InSquad(e) || EHGet(g->m_hEnemy) < 0)
		return;
	leader = Sqm_MySquadLeader(e);
	if (Mon_HasConditions((CBaseMonster *)g, bits_COND_SEE_ENEMY))
		SQM(leader)->m_flLastEnemySightTime = gtime();	/* the squad's last sighting */
	else if (gtime() - SQM(leader)->m_flLastEnemySightTime > 5)
		SQM(leader)->m_fEnemyEluded = 1;	/* been a while */
}

/* grunts throw / shoot grenades at an enemy they cannot see */
static int Hg_FCanCheckAttacks(int e)
{
	return !Mon_HasConditions(MON(e), bits_COND_ENEMY_TOOFAR);
}

static int Hg_CheckMeleeAttack1(int e, float flDot, float flDist)
{
	int en = EHGet(HG(e)->m_hEnemy), c;

	if (en < 0 || !mon_is_monster(en))
		return 0;
	c = Classify(en);
	return flDist <= 64 && flDot >= 0.7f && c != CLASS_ALIEN_BIOWEAPON &&
		c != CLASS_PLAYER_BIOWEAPON;
}

static void Hg_GetGunPosition(int e, float out[3])
{
	VecCopy(ent_cv(e)->origin, out);
	out[2] += HG(e)->m_fStanding ? 60 : 48;
}

/* the machine gun only with the enemy in sight (FCanCheckAttacks lets an
 * occluded enemy through for the grenades) */
static int Hg_CheckRangeAttack1(int e, float flDot, float flDist)
{
	CHGrunt		*g = HG(e);
	int		en = EHGet(g->m_hEnemy);
	col_trace_t	tr;
	float		src[3], tgt[3];

	if (en < 0 || Mon_HasConditions((CBaseMonster *)g, bits_COND_ENEMY_OCCLUDED) ||
		flDist > 2048 || flDot < 0.5f || !Sqm_NoFriendlyFire(e))
		return 0;
	if (!IsPlayer(en) && flDist <= 64)
		return 0;	/* kick nonclients, don't shoot them */
	Hg_GetGunPosition(e, src);
	BodyTargetIn(en, e, src, tgt);
	/* a bullet from the gun hits the enemy before the world */
	UTIL_TraceLineGlass(src, tgt, ignore_monsters, ignore_glass, e, &tr);
	return tr.frac == 1.0f;
}

/* the grenade attack */
static int Hg_CheckRangeAttack2(int e, float flDot, float flDist)
{
	CHGrunt	*g = HG(e);
	int	en = EHGet(g->m_hEnemy);
	float	tgt[3], bt[3], d[3], src[3], toss[3];
	int	ok;

	(void)flDot; (void)flDist;
	if (!(g->m_weapons & (HGRUNT_HANDGRENADE | HGRUNT_GRENADELAUNCHER)) || en < 0)
		return 0;
	if (g->anim.m_flGroundSpeed != 0)
	{
		g->m_fThrowGrenade = 0;	/* only when not moving */
		return 0;
	}
	if (gtime() < g->m_flNextGrenadeCheck)
		return g->m_fThrowGrenade;	/* assume things haven't changed much */
	if (!(ent_cv(en)->flags & FL_ONGROUND) && waterlevel_of(en) == 0 &&
		g->m_vecEnemyLKP[2] > ent_cv(e)->absmax[2])
	{
		/* don't throw grenades at anything that isn't on the ground */
		g->m_fThrowGrenade = 0;
		return 0;
	}
	if (g->m_weapons & HGRUNT_HANDGRENADE)
	{
		if (RANDOM_LONG(0, 1))	/* magically know where the feet are */
		{
			OriginIn(en, e, tgt);
			tgt[2] = ent_cv(en)->absmin[2] + tgt[2] - ent_cv(en)->origin[2];
		}
		else			/* where they were last seen */
			VecCopy(g->m_vecEnemyLKP, tgt);
	}
	else
	{
		BodyTarget(en, ent_cv(e)->origin, bt);
		VecSub(bt, ent_cv(en)->origin, d);
		VecAdd(g->m_vecEnemyLKP, d, tgt);
		if (Mon_HasConditions((CBaseMonster *)g, bits_COND_SEE_ENEMY))
		{
			VecSub(tgt, ent_cv(e)->origin, d);
			VecMA(tgt, VecLen(d) / mon_skill(sk_hgruntGrenadeSpeed), velocity_of(en), tgt);
		}
	}
	/* squad members near the impact area: don't, and don't check for a while */
	if (Sqm_InSquad(e) && Sqm_SquadMemberInRange(e, tgt, 256))
	{
		g->m_flNextGrenadeCheck = gtime() + 1;
		g->m_fThrowGrenade = 0;
	}
	VecSub(tgt, ent_cv(e)->origin, d);
	if (VecLen2D(d) <= 256)
	{
		g->m_flNextGrenadeCheck = gtime() + 1;	/* don't blow myself up */
		g->m_fThrowGrenade = 0;
		return 0;
	}
	Hg_GetGunPosition(e, src);
	if (g->m_weapons & HGRUNT_HANDGRENADE)
	{
		ok = VecCheckToss(e, src, tgt, 0.5f, toss);
		g->m_flNextGrenadeCheck = ok ? gtime() : gtime() + 1;
	}
	else
	{
		ok = VecCheckThrow(e, src, tgt, mon_skill(sk_hgruntGrenadeSpeed), 0.5f, toss);
		g->m_flNextGrenadeCheck = gtime() + (ok ? 0.3f : 1);
	}
	if (ok)
		VecCopy(toss, g->m_vecTossVelocity);
	g->m_fThrowGrenade = (uint8_t)ok;
	return ok;
}

/* the helmet absorbs head shots */
static void Hg_TraceAttack(int e, int attacker, float dmg, const float dir[3],
	const col_trace_t *tr, int bits)
{
	col_trace_t t = *tr;

	if (t.hitgroup == 11)
	{
		if (Mon_GetBodygroup(e, HEAD_GROUP) == HEAD_GRUNT &&
			(bits & (DMG_BULLET | DMG_SLASH | DMG_BLAST | DMG_CLUB)))
		{
			dmg -= 20;
			if (dmg <= 0)
			{
				UTIL_Ricochet(t.end, 1.0f);
				dmg = 0.01f;
			}
		}
		t.hitgroup = HITGROUP_HEAD;
	}
	CBaseMonster_TraceAttack(e, attacker, dmg, dir, &t, bits);
}

/* hurt: no longer in a safe place */
static int Hg_TakeDamage(int e, int inflictor, int attacker, float flDamage, int bits)
{
	Mon_Forget(MON(e), bits_MEMORY_INCOVER);
	return CBaseMonster_TakeDamage(e, inflictor, attacker, flDamage, bits);
}

static void Hg_SetYawSpeed(int e)
{
	int ys;

	switch (MON(e)->m_Activity)
	{
	case ACT_IDLE:
	case ACT_RUN:
		ys = 150;
		break;
	case ACT_WALK:
	case ACT_TURN_LEFT:
	case ACT_TURN_RIGHT:
		ys = 180;
		break;
	case ACT_RANGE_ATTACK1:
	case ACT_RANGE_ATTACK2:
	case ACT_MELEE_ATTACK1:
	case ACT_MELEE_ATTACK2:
		ys = 120;
		break;
	case ACT_GLIDE:
	case ACT_FLY:
		ys = 30;
		break;
	default:
		ys = 90;
		break;
	}
	ent_phys(e)->yaw_speed = (float)ys;
}

static void Hg_IdleSound(int e)
{
	if (!Hg_FOkToSpeak(e) || !(g_fGruntQuestion || RANDOM_LONG(0, 1)))
		return;
	if (!g_fGruntQuestion)
	{
		/* ask a question or make a statement */
		switch (RANDOM_LONG(0, 2))
		{
		case 0:	/* check in */
			Hg_Say(e, RESID(GRP_HG_CHECK));
			g_fGruntQuestion = 1;
			break;
		case 1:	/* question */
			Hg_Say(e, RESID(GRP_HG_QUEST));
			g_fGruntQuestion = 2;
			break;
		case 2:	/* statement */
			Hg_Say(e, RESID(GRP_HG_IDLE));
			break;
		}
	}
	else
	{
		switch (g_fGruntQuestion)
		{
		case 1:	/* check in */
			Hg_Say(e, RESID(GRP_HG_CLEAR));
			break;
		case 2:	/* question */
			Hg_Say(e, RESID(GRP_HG_ANSWER));
			break;
		}
		g_fGruntQuestion = 0;
	}
	Hg_JustSpoke(e);
}

static void Hg_CheckAmmo(int e)
{
	if (MON(e)->m_cAmmoLoaded <= 0)
		Mon_SetConditions(MON(e), bits_COND_NO_AMMO_LOADED);
}

static int Hg_Classify(int e) { (void)e; return CLASS_HUMAN_MILITARY; }

static int Hg_Kick(int e)
{
	col_trace_t	tr;
	float		start[3], end[3];

	UTIL_MakeVectors(ent_cv(e)->angles);
	VecCopy(ent_cv(e)->origin, start);
	start[2] += (ent_cv(e)->maxs[2] - ent_cv(e)->mins[2]) * 0.5f;
	VecMA(start, 70, gpGlobals.v_forward, end);
	UTIL_TraceHull(start, end, dont_ignore_monsters, head_hull, e, &tr);
	return tr.ent;
}

static void Hg_FireAtEnemy(int e, int shotgun)
{
	CHGrunt	*g = HG(e);
	float	so[3], dir[3], shell[3], at[3], ang[3];
	uint64_t t0;

	if (EHGet(g->m_hEnemy) < 0)
		return;
	t0 = Mon_PzBegin();
	Hg_GetGunPosition(e, so);
	Mon_ShootAtEnemy(e, so, dir);
	Mon_PzEnd(MPZ_SHOOT, t0);
	UTIL_MakeVectors(ent_cv(e)->angles);
	VecScale(gpGlobals.v_right, RANDOM_FLOAT(40, 90), shell);
	VecMA(shell, RANDOM_FLOAT(75, 200), gpGlobals.v_up, shell);
	VecMA(shell, RANDOM_FLOAT(-40, 40), gpGlobals.v_forward, shell);
	VecMA(so, -24, dir, at);
	if (shotgun)
	{
		t0 = Mon_PzBegin();
		EjectBrass(at, shell, ent_cv(e)->angles[1], RESID(W_SHOTGUNSHELL), TE_BOUNCE_SHOTSHELL);
		Mon_PzEnd(MPZ_BRASS, t0);
		FireBullets(e, (int)mon_skill(sk_hgruntShotgunPellets), so, dir, VECTOR_CONE_15DEGREES,
			2048, BULLET_PLAYER_BUCKSHOT, 0, 0, -1);
	}
	else
	{
		t0 = Mon_PzBegin();
		EjectBrass(at, shell, ent_cv(e)->angles[1], RESID(W_SHELL), TE_BOUNCE_SHELL);
		Mon_PzEnd(MPZ_BRASS, t0);
		FireBullets(e, 1, so, dir, VECTOR_CONE_10DEGREES, 2048, BULLET_MONSTER_MP5, 4, 0, -1);
	}
	ent_rnd(e)->effects |= EF_MUZZLEFLASH;
	g->m_cAmmoLoaded--;
	t0 = Mon_PzBegin();
	UTIL_VecToAngles(dir, ang);
	gs_set_blending(ent_rnd(e)->model, ent_rnd(e)->sequence, ent_rnd(e)->blending, 0, ang[0]);
	Mon_PzEnd(MPZ_SHOOT, t0);
}

static void Hg_HandleAnimEvent(int e, const MonsterEvent_t *ev)
{
	CHGrunt	*g = HG(e);
	float	pos[3], ang[3] = { 0, 0, 0 }, v[3];
	int	hurt;

	switch (ev->event)
	{
	case HGRUNT_AE_DROP_GUN:
		gs_attachment(e, 0, pos);
		Mon_SetBodygroup(e, GUN_GROUP, GUN_NONE);	/* no gun */
		Mon_DropItem(e, (g->m_weapons & HGRUNT_SHOTGUN) ? CLS_WEAPON_SHOTGUN : CLS_WEAPON_9MMAR,
			pos, ang);
		if (g->m_weapons & HGRUNT_GRENADELAUNCHER)
		{
			BodyTarget(e, ent_cv(e)->origin, pos);
			Mon_DropItem(e, CLS_AMMO_ARGRENADES, pos, ang);
		}
		break;
	case HGRUNT_AE_RELOAD:
		EMIT_SOUND(e, CHAN_WEAPON, RESID(HG_RELOAD1), 1, ATTN_NORM);
		g->m_cAmmoLoaded = g->m_cClipSize;
		Mon_ClearConditions((CBaseMonster *)g, bits_COND_NO_AMMO_LOADED);
		break;
	case HGRUNT_AE_GREN_TOSS:
		UTIL_MakeVectors(ent_cv(e)->angles);
		Hg_GetGunPosition(e, pos);
		CGrenade_ShootTimed(e, pos, g->m_vecTossVelocity, 3.5f);
		g->m_fThrowGrenade = 0;
		g->m_flNextGrenadeCheck = gtime() + 6;	/* six seconds before looking again */
		break;
	case HGRUNT_AE_GREN_LAUNCH:
		EMIT_SOUND(e, CHAN_WEAPON, RESID(W_GLAUNCHER), 0.8f, ATTN_NORM);
		Hg_GetGunPosition(e, pos);
		CGrenade_ShootContact(e, pos, g->m_vecTossVelocity);
		g->m_fThrowGrenade = 0;
		if (settings.skill == SKILL_HARD)
			g->m_flNextGrenadeCheck = gtime() + RANDOM_FLOAT(2, 5);
		else
			g->m_flNextGrenadeCheck = gtime() + 6;
		break;
	case HGRUNT_AE_GREN_DROP:
		UTIL_MakeVectors(ent_cv(e)->angles);
		VecMA(ent_cv(e)->origin, 17, gpGlobals.v_forward, pos);
		VecMA(pos, -27, gpGlobals.v_right, pos);
		VecMA(pos, 6, gpGlobals.v_up, pos);
		VecSet(v, 0, 0, 0);
		CGrenade_ShootTimed(e, pos, v, 3);
		break;
	case HGRUNT_AE_BURST1:
		{
			uint64_t t0;

			if (g->m_weapons & HGRUNT_9MMAR)
			{
				Hg_FireAtEnemy(e, 0);
				/* the first round of the burst plays the sound */
				t0 = Mon_PzBegin();
				EMIT_SOUND(e, CHAN_WEAPON, RANDOM_LONG(0, 1) ? RESID(HG_MGUN1) : RESID(HG_MGUN2),
					1, ATTN_NORM);
			}
			else
			{
				Hg_FireAtEnemy(e, 1);
				t0 = Mon_PzBegin();
				EMIT_SOUND(e, CHAN_WEAPON, RESID(W_SBARREL1), 1, ATTN_NORM);
			}
			CSoundEnt_InsertSound(bits_SOUND_COMBAT, ent_cv(e)->origin, 384, 0.3f);
			Mon_PzEnd(MPZ_ESND, t0);
		}
		break;
	case HGRUNT_AE_BURST2:
	case HGRUNT_AE_BURST3:
		Hg_FireAtEnemy(e, 0);
		break;
	case HGRUNT_AE_KICK:
		hurt = Hg_Kick(e);
		if (hurt > 0)
		{
			UTIL_MakeVectors(ent_cv(e)->angles);
			VecMA(velocity_of(hurt), 100, gpGlobals.v_forward, v);
			VecMA(v, 50, gpGlobals.v_up, v);
			if (IsPlayer(hurt))
			{
				phy_player()->punchangle[0] = 15;
				VecCopy(v, phy_player()->velocity);
			}
			else
				ent_set_velocity(hurt, v);
			TakeDamage(hurt, e, e, mon_skill(sk_hgruntDmgKick), DMG_CLUB);
		}
		break;
	case HGRUNT_AE_CAUGHT_ENEMY:
		if (Hg_FOkToSpeak(e))
		{
			Hg_Say(e, RESID(GRP_HG_ALERT));
			Hg_JustSpoke(e);
		}
		/* fall through */
	default:
		CBaseMonster_HandleAnimEvent(e, ev);
		break;
	}
}

static void Hg_StartTask(int e, const Task_t *t)
{
	CHGrunt *g = HG(e);

	g->m_iTaskStatus = TASKSTATUS_RUNNING;
	switch (t->iTask)
	{
	case TASK_GRUNT_CHECK_FIRE:
		if (!Sqm_NoFriendlyFire(e))
			Mon_SetConditions((CBaseMonster *)g, bits_COND_GRUNT_NOFIRE);
		Mon_TaskComplete((CBaseMonster *)g);
		break;
	case TASK_GRUNT_SPEAK_SENTENCE:
		Hg_SpeakSentence(e);
		Mon_TaskComplete((CBaseMonster *)g);
		break;
	case TASK_WALK_PATH:
	case TASK_RUN_PATH:
		/* no longer covered once moving */
		Mon_Forget((CBaseMonster *)g, bits_MEMORY_INCOVER);
		CBaseMonster_StartTask(e, t);
		break;
	case TASK_RELOAD:
		g->m_IdealActivity = ACT_RELOAD;
		break;
	case TASK_GRUNT_FACE_TOSS_DIR:
		break;
	case TASK_FACE_IDEAL:
	case TASK_FACE_ENEMY:
		CBaseMonster_StartTask(e, t);
		if (ent_cv(e)->movetype == MOVETYPE_FLY)
			g->m_IdealActivity = ACT_GLIDE;
		break;
	default:
		CBaseMonster_StartTask(e, t);
		break;
	}
}

static void Hg_RunTask(int e, const Task_t *t)
{
	CHGrunt	*g = HG(e);
	float	p[3];

	if (t->iTask == TASK_GRUNT_FACE_TOSS_DIR)
	{
		/* face a point along the toss vector */
		VecMA(ent_cv(e)->origin, 64, g->m_vecTossVelocity, p);
		Mon_MakeIdealYaw(e, p);
		Mon_ChangeYaw(e, (int)ent_phys(e)->yaw_speed);
		if (Mon_FacingIdeal(e))
			g->m_iTaskStatus = TASKSTATUS_COMPLETE;
		return;
	}
	CBaseMonster_RunTask(e, t);
}

static void Hg_PainSound(int e)
{
	CHGrunt *g = HG(e);
	int i;

	if (gtime() <= g->m_flNextPainTime)
		return;
	i = RANDOM_LONG(0, 6);
	if (i < (int)ARRAYSIZE(hgPain))
		EMIT_SOUND(e, CHAN_VOICE, res_def(hgPain[i]), 1, ATTN_NORM);
	g->m_flNextPainTime = gtime() + 1;
}

static void Hg_DeathSound(int e)
{
	EMIT_SOUND(e, CHAN_VOICE, RANDOM_SOUND_ARRAY(hgDie), 1, ATTN_IDLE);
}

static void Hg_SetActivity(int e, int act)
{
	CHGrunt		*g = HG(e);
	ent_rnd_t	*rn = ent_rnd(e);
	int		seq;

	switch (act)
	{
	case ACT_RANGE_ATTACK1:
		if (g->m_weapons & HGRUNT_9MMAR)
			seq = Mon_LookupSequence(e, g->m_fStanding ? "standing_mp5" : "crouching_mp5");
		else
			seq = Mon_LookupSequence(e, g->m_fStanding ? "standing_shotgun" :
				"crouching_shotgun");
		break;
	case ACT_RANGE_ATTACK2:
		seq = Mon_LookupSequence(e, (g->m_weapons & HGRUNT_HANDGRENADE) ? "throwgrenade" :
			"launchgrenade");
		break;
	case ACT_RUN:
		seq = Mon_LookupActivity(e, g->health <= HGRUNT_LIMP_HEALTH ? ACT_RUN_HURT : act);
		break;
	case ACT_WALK:
		seq = Mon_LookupActivity(e, g->health <= HGRUNT_LIMP_HEALTH ? ACT_WALK_HURT : act);
		break;
	case ACT_IDLE:
		if (g->m_MonsterState == MONSTERSTATE_COMBAT)
			act = ACT_IDLE_ANGRY;
		seq = Mon_LookupActivity(e, act);
		break;
	default:
		seq = Mon_LookupActivity(e, act);
		break;
	}
	g->m_Activity = act;	/* the activity, even without a sequence */
	if (seq > ACTIVITY_NOT_AVAILABLE)
	{
		if (rn->sequence != seq || !g->anim.m_fSequenceLoops)
			rn->frame = 0;
		rn->sequence = (uint16_t)seq;
		Mon_ResetSequenceInfo(e);
		Hg_SetYawSpeed(e);
	}
	else
	{
		log_printf("monster_human_grunt has no sequence for act:%d\n", act);
		rn->sequence = 0;
	}
}

static const Schedule_t *Hg_GetScheduleOfType(int e, int type);

static const Schedule_t *Hg_GetSchedule(int e)
{
	CHGrunt		*g = HG(e);
	CBaseMonster	*m = (CBaseMonster *)g;
	CSound		*snd;
	int		en, c;

	g->m_iSentence = HGRUNT_SENT_NONE;	/* clear old sentence */
	/* repelling down a rope */
	if (ent_cv(e)->movetype == MOVETYPE_FLY && g->m_MonsterState != MONSTERSTATE_PRONE)
	{
		if (ent_cv(e)->flags & FL_ONGROUND)
		{
			/* just landed */
			ent_set_movetype(e, MOVETYPE_STEP);
			return Hg_GetScheduleOfType(e, SCHED_GRUNT_REPEL_LAND);
		}
		/* repel down a rope */
		if (g->m_MonsterState == MONSTERSTATE_COMBAT)
			return Hg_GetScheduleOfType(e, SCHED_GRUNT_REPEL_ATTACK);
		return Hg_GetScheduleOfType(e, SCHED_GRUNT_REPEL);
	}
	/* grunts place HIGH priority on running away from danger sounds */
	if (Mon_HasConditions(m, bits_COND_HEAR_SOUND))
	{
		snd = Mon_PBestSound(e);
		if (snd && (snd->m_iType & bits_SOUND_DANGER))
		{
			/* dangerous sound nearby */
			if (Hg_FOkToSpeak(e))
			{
				Hg_Say(e, RESID(GRP_HG_GREN));
				Hg_JustSpoke(e);
			}
			return Hg_GetScheduleOfType(e, SCHED_TAKE_COVER_FROM_BEST_SOUND);
		}
	}
	if (g->m_MonsterState == MONSTERSTATE_COMBAT)
	{
		en = EHGet(g->m_hEnemy);
		if (Mon_HasConditions(m, bits_COND_ENEMY_DEAD))
			return CBaseMonster_GetSchedule(e);
		if (Mon_HasConditions(m, bits_COND_NEW_ENEMY))
		{
			if (Sqm_InSquad(e))
			{
				SQM(Sqm_MySquadLeader(e))->m_fEnemyEluded = 0;
				if (!Sqm_IsLeader(e))
					return Hg_GetScheduleOfType(e, SCHED_TAKE_COVER_FROM_ENEMY);
				/* the leader barks an alert */
				if (Hg_FOkToSpeak(e))
				{
					if (en >= 0 && IsPlayer(en))
						Hg_Say(e, RESID(GRP_HG_ALERT));
					else if (en >= 0)
					{
						c = Classify(en);
						if (c != CLASS_PLAYER_ALLY && c != CLASS_HUMAN_PASSIVE &&
							c != CLASS_MACHINE)
							Hg_Say(e, RESID(GRP_HG_MONST));
					}
					Hg_JustSpoke(e);
				}
				if (Mon_HasConditions(m, bits_COND_CAN_RANGE_ATTACK1))
					return Hg_GetScheduleOfType(e, SCHED_GRUNT_SUPPRESS);
				return Hg_GetScheduleOfType(e, SCHED_GRUNT_ESTABLISH_LINE_OF_FIRE);
			}
		}
		else if (Mon_HasConditions(m, bits_COND_NO_AMMO_LOADED))
			return Hg_GetScheduleOfType(e, SCHED_GRUNT_COVER_AND_RELOAD);
		else if (Mon_HasConditions(m, bits_COND_LIGHT_DAMAGE))
		{
			/* hurt but able to fight: 90% cover, else flinch */
			if (RANDOM_LONG(0, 99) <= 90 && en >= 0)
			{
				if (Hg_FOkToSpeak(e))
					g->m_iSentence = HGRUNT_SENT_COVER;
				return Hg_GetScheduleOfType(e, SCHED_TAKE_COVER_FROM_ENEMY);
			}
			return Hg_GetScheduleOfType(e, SCHED_SMALL_FLINCH);
		}
		else if (Mon_HasConditions(m, bits_COND_CAN_MELEE_ATTACK1))
			return Hg_GetScheduleOfType(e, SCHED_MELEE_ATTACK1);
		else if ((g->m_weapons & HGRUNT_GRENADELAUNCHER) &&
			Mon_HasConditions(m, bits_COND_CAN_RANGE_ATTACK2) &&
			Sqm_OccupySlot(e, bits_SLOTS_HGRUNT_GRENADE))
			return Hg_GetScheduleOfType(e, SCHED_RANGE_ATTACK2);	/* launch a grenade */
		else if (Mon_HasConditions(m, bits_COND_CAN_RANGE_ATTACK1))
		{
			if (Sqm_InSquad(e) && SQM(Sqm_MySquadLeader(e))->m_fEnemyEluded &&
				!Mon_HasConditions(m, bits_COND_ENEMY_FACING_ME))
			{
				/* the enemy eluded the squad and is not facing me: signal */
				SQM(Sqm_MySquadLeader(e))->m_fEnemyEluded = 0;
				return Hg_GetScheduleOfType(e, SCHED_GRUNT_FOUND_ENEMY);
			}
			if (Sqm_OccupySlot(e, bits_SLOTS_HGRUNT_ENGAGE))
				return Hg_GetScheduleOfType(e, SCHED_RANGE_ATTACK1);
			if (Mon_HasConditions(m, bits_COND_CAN_RANGE_ATTACK2) &&
				Sqm_OccupySlot(e, bits_SLOTS_HGRUNT_GRENADE))
				return Hg_GetScheduleOfType(e, SCHED_RANGE_ATTACK2);
			return Hg_GetScheduleOfType(e, SCHED_TAKE_COVER_FROM_ENEMY);
		}
		else if (Mon_HasConditions(m, bits_COND_ENEMY_OCCLUDED))
		{
			if (Mon_HasConditions(m, bits_COND_CAN_RANGE_ATTACK2) &&
				Sqm_OccupySlot(e, bits_SLOTS_HGRUNT_GRENADE))
			{
				/* flush the enemy out with a grenade */
				if (Hg_FOkToSpeak(e))
				{
					Hg_Say(e, RESID(GRP_HG_THROW));
					Hg_JustSpoke(e);
				}
				return Hg_GetScheduleOfType(e, SCHED_RANGE_ATTACK2);
			}
			if (Sqm_OccupySlot(e, bits_SLOTS_HGRUNT_ENGAGE))
			{
				if (Hg_FOkToSpeak(e))
					g->m_iSentence = HGRUNT_SENT_CHARGE;
				return Hg_GetScheduleOfType(e, SCHED_GRUNT_ESTABLISH_LINE_OF_FIRE);
			}
			/* hide, taunt */
			if (Hg_FOkToSpeak(e) && RANDOM_LONG(0, 1))
			{
				Hg_Say(e, RESID(GRP_HG_TAUNT));
				Hg_JustSpoke(e);
			}
			return Hg_GetScheduleOfType(e, SCHED_STANDOFF);
		}
		if (Mon_HasConditions(m, bits_COND_SEE_ENEMY) &&
			!Mon_HasConditions(m, bits_COND_CAN_RANGE_ATTACK1))
			return Hg_GetScheduleOfType(e, SCHED_GRUNT_ESTABLISH_LINE_OF_FIRE);
	}
	return CBaseMonster_GetSchedule(e);
}

static void repel_fall(int e)
{
	float v[3];

	VecCopy(ent_mot(e)->velocity, v);
	if (v[2] > -128)
	{
		v[2] -= 32;
		ent_set_velocity(e, v);
	}
}

static const Schedule_t *Hg_GetScheduleOfType(int e, int type)
{
	CHGrunt		*g = HG(e);
	CBaseMonster	*m = (CBaseMonster *)g;
	int		en;

	switch (type)
	{
	case SCHED_TAKE_COVER_FROM_ENEMY:
		if (Sqm_InSquad(e))
		{
			if (settings.skill == SKILL_HARD &&
				Mon_HasConditions(m, bits_COND_CAN_RANGE_ATTACK2) &&
				Sqm_OccupySlot(e, bits_SLOTS_HGRUNT_GRENADE))
			{
				if (Hg_FOkToSpeak(e))
				{
					Hg_Say(e, RESID(GRP_HG_THROW));
					Hg_JustSpoke(e);
				}
				return slGruntTossGrenadeCover;
			}
			return slGruntTakeCover;
		}
		return RANDOM_LONG(0, 1) ? slGruntTakeCover : slGruntGrenadeCover;
	case SCHED_TAKE_COVER_FROM_BEST_SOUND:
		return slGruntTakeCoverFromBestSound;
	case SCHED_GRUNT_TAKECOVER_FAILED:
		if (Mon_HasConditions(m, bits_COND_CAN_RANGE_ATTACK1) &&
			Sqm_OccupySlot(e, bits_SLOTS_HGRUNT_ENGAGE))
			return Hg_GetScheduleOfType(e, SCHED_RANGE_ATTACK1);
		return Hg_GetScheduleOfType(e, SCHED_FAIL);
	case SCHED_GRUNT_ELOF_FAIL:
		/* human grunt is unable to move to a position that allows him to attack */
		return Hg_GetScheduleOfType(e, SCHED_TAKE_COVER_FROM_ENEMY);
	case SCHED_GRUNT_ESTABLISH_LINE_OF_FIRE:
		return slGruntEstablishLineOfFire;
	case SCHED_RANGE_ATTACK1:
		/* randomly stand or crouch */
		if (RANDOM_LONG(0, 9) == 0)
			g->m_fStanding = (uint8_t)RANDOM_LONG(0, 1);
		return g->m_fStanding ? slGruntRangeAttack1B : slGruntRangeAttack1A;
	case SCHED_RANGE_ATTACK2:
		return slGruntRangeAttack2;
	case SCHED_COMBAT_FACE:
		return slGruntCombatFace;
	case SCHED_GRUNT_WAIT_FACE_ENEMY:
		return slGruntWaitInCover;
	case SCHED_GRUNT_SWEEP:
		return slGruntSweep;
	case SCHED_GRUNT_COVER_AND_RELOAD:
		return slGruntHideReload;
	case SCHED_GRUNT_FOUND_ENEMY:
		return slGruntFoundEnemy;
	case SCHED_VICTORY_DANCE:
		if (Sqm_InSquad(e) && !Sqm_IsLeader(e))
			return slGruntFail;
		return slGruntVictoryDance;
	case SCHED_GRUNT_SUPPRESS:
		en = EHGet(g->m_hEnemy);
		if (en >= 0 && IsPlayer(en) && g->m_fFirstEncounter)
		{
			g->m_fFirstEncounter = 0;	/* after this, we squad up */
			return slGruntSignalSuppress;
		}
		return slGruntSuppress;
	case SCHED_FAIL:
		/* no fail schedule specified, so pick a good generic one */
		return EHGet(g->m_hEnemy) >= 0 ? slGruntCombatFail : slGruntFail;
	case SCHED_GRUNT_REPEL:
		repel_fall(e);
		return slGruntRepel;
	case SCHED_GRUNT_REPEL_ATTACK:
		repel_fall(e);
		return slGruntRepelAttack;
	case SCHED_GRUNT_REPEL_LAND:
		return slGruntRepelLand;
	default:
		return CSquadMonster_GetScheduleOfType(e, type);
	}
}

static void Hg_Precache(int e)
{
	static const int16_t pre[] = {
		R_HG_MGUN1, R_HG_MGUN2, R_HG_DIE1, R_HG_DIE2, R_HG_DIE3, R_HG_PAIN1, R_HG_PAIN2,
		R_HG_PAIN3, R_HG_PAIN4, R_HG_PAIN5, R_HG_RELOAD1, R_W_GLAUNCHER, R_W_SBARREL1,
		R_MON_CLAW_MISS2, R_W_SHELL, R_W_SHOTGUNSHELL,
	};

	PRECACHE(RESID(MON_HGRUNT_MDL));
	PRECACHE_SOUND_ARRAY(pre);
	/* voice pitch */
	HG(e)->m_voicePitch = RANDOM_LONG(0, 1) ? 109 + RANDOM_LONG(0, 7) : 100;
}

static int CHGrunt_Spawn(int e, gid_t16 gid)
{
	CBaseMonster	*m = Mon_SpawnKeys(e, gid);
	CHGrunt		*g = (CHGrunt *)m;

	Sqm_Init(e);
	g->m_weapons = gid != GID_NONE ? (int)KeyF(gid, K_WEAPONS, 0) : 0;
	Hg_Precache(e);
	ent_set_model(e, RESID(MON_HGRUNT_MDL));
	ent_set_size(e, VEC_HUMAN_HULL_MIN, VEC_HUMAN_HULL_MAX);
	ent_set_solid(e, SOLID_SLIDEBOX);
	ent_set_movetype(e, MOVETYPE_STEP);
	m->m_bloodColor = BLOOD_COLOR_RED;
	ent_rnd(e)->effects = 0;
	m->health = mon_skill(sk_hgruntHealth);
	m->m_flFieldOfView = 0.2f;
	m->m_MonsterState = MONSTERSTATE_NONE;
	g->m_flNextGrenadeCheck = gtime() + 1;
	g->m_flNextPainTime = gtime();
	g->m_iSentence = HGRUNT_SENT_NONE;
	m->m_afCapability = bits_CAP_SQUAD | bits_CAP_TURN_HEAD | bits_CAP_DOORS_GROUP;
	g->m_fEnemyEluded = 0;
	g->m_fFirstEncounter = 1;	/* no enemy encountered yet */
	VecSet(m->m_HackedGunPos, 0, 0, 55);
	if (g->m_weapons == 0)
		g->m_weapons = HGRUNT_9MMAR | HGRUNT_HANDGRENADE;	/* the original values */
	if (g->m_weapons & HGRUNT_SHOTGUN)
	{
		Mon_SetBodygroup(e, GUN_GROUP, GUN_SHOTGUN);
		g->m_cClipSize = 8;
	}
	else
		g->m_cClipSize = GRUNT_CLIP_SIZE;
	m->m_cAmmoLoaded = g->m_cClipSize;
	ent_cv(e)->skin = RANDOM_LONG(0, 99) < 80 ? 0 : 1;	/* light / dark skin */
	if (g->m_weapons & HGRUNT_SHOTGUN)
		Mon_SetBodygroup(e, HEAD_GROUP, HEAD_SHOTGUN);
	else if (g->m_weapons & HGRUNT_GRENADELAUNCHER)
	{
		Mon_SetBodygroup(e, HEAD_GROUP, HEAD_M203);
		ent_cv(e)->skin = 1;	/* always dark skin */
	}
	g_talkWaitTime = 0;
	Mon_MonsterInit(e);
	return 0;
}

static const game_vt_t hg_vt = {
	MON_GAME_VT_BASE,
	.TraceAttack = Hg_TraceAttack,
	.TakeDamage = Hg_TakeDamage,
	.Killed = CSquadMonster_Killed,
	.IRelationship = Hg_IRelationship,
	.Classify = Hg_Classify,
};

static const mon_vt_t hg_mvt = {
	.squad = 1,
	.SetYawSpeed = Hg_SetYawSpeed,
	.ISoundMask = Hg_ISoundMask,
	.HandleAnimEvent = Hg_HandleAnimEvent,
	.FCanCheckAttacks = Hg_FCanCheckAttacks,
	.CheckMeleeAttack1 = Hg_CheckMeleeAttack1,
	.CheckRangeAttack1 = Hg_CheckRangeAttack1,
	.CheckRangeAttack2 = Hg_CheckRangeAttack2,
	.CheckAmmo = Hg_CheckAmmo,
	.SetActivity = Hg_SetActivity,
	.StartTask = Hg_StartTask,
	.RunTask = Hg_RunTask,
	.DeathSound = Hg_DeathSound,
	.PainSound = Hg_PainSound,
	.IdleSound = Hg_IdleSound,
	.GetGunPosition = Hg_GetGunPosition,
	.PrescheduleThink = Hg_PrescheduleThink,
	.GibMonster = Hg_GibMonster,
	.GetSchedule = Hg_GetSchedule,
	.GetScheduleOfType = Hg_GetScheduleOfType,
	.CheckEnemy = CSquadMonster_CheckEnemy,
	.StartMonster = CSquadMonster_StartMonster,
	.ScheduleChange = CSquadMonster_ScheduleChange,
	.FValidateCover = CSquadMonster_FValidateCover,
	.GetIdealState = CSquadMonster_GetIdealState,
};

/* HLSDK CHGrunt::m_SaveData */
static const sv_field_t f_CHGrunt[] = {
	SVF(CHGrunt, m_flNextGrenadeCheck, SV_TIME, 0),
	SVF(CHGrunt, m_flNextPainTime, SV_TIME, 0),
	SVF(CHGrunt, m_vecTossVelocity, SV_VEC, 0),
	SVF(CHGrunt, m_fThrowGrenade, SV_U8, 0),
	SVF(CHGrunt, m_fStanding, SV_U8, 0),
	SVF(CHGrunt, m_fFirstEncounter, SV_U8, 0),
	SVF(CHGrunt, m_iSentence, SV_U8, 0),
	SVF(CHGrunt, m_cClipSize, SV_I32, SVK),
	SVF(CHGrunt, m_voicePitch, SV_I32, 0),
	SVF(CHGrunt, m_weapons, SV_I32, SVK),
};
static const sv_table_t sv_CHGrunt = SV_TABLE(&sv_CSquadMonster, f_CHGrunt);

const game_class_t gc_MONSTER_HUMAN_GRUNT = {
	.size = sizeof(CHGrunt),
	.Spawn = CHGrunt_Spawn,
	.vt = &hg_vt,
	.ext = &hg_mvt,
	.save = &sv_CHGrunt,
};

/* ---- monster_grunt_repel (CHGruntRepel) ---------------------------------------------------- */

void CHGruntRepel_RepelUse(int e, int activator, int caller, int useType, float value)
{
	col_trace_t	tr;
	float		end[3], v[3], top[3], life;
	int		g;

	(void)activator; (void)caller; (void)useType; (void)value;
	VecCopy(ent_cv(e)->origin, end);
	end[2] -= 4096;
	UTIL_TraceLine(ent_cv(e)->origin, end, dont_ignore_monsters, e, &tr);
	g = CBaseEntity_Create(CLS_MONSTER_HUMAN_GRUNT, ent_cv(e)->origin, ent_cv(e)->angles, -1);
	if (g < 0)
	{
		UTIL_Remove(e);
		return;
	}
	ent_set_movetype(g, MOVETYPE_FLY);
	VecSet(v, 0, 0, RANDOM_FLOAT(-196, -128));
	ent_set_velocity(g, v);
	Mon_SetActivity(g, ACT_GLIDE);
	/* UNDONE: position? */
	VecCopy(tr.end, MON(g)->m_vecLastPosition);
	/* the rope: point to the grunt, removed when he lands */
	VecCopy(ent_cv(e)->origin, top);
	top[2] += 112;
	life = -4096.0f * tr.frac / v[2] + 0.5f;
	fx_beam(-1, top, g, NULL, RESID(MON_ROPE_SPR), 10, 0, 255, 255, 255, 255, 0,
		(int)(life * 10 + 0.5f));
	UTIL_Remove(e);
}

static int CHGruntRepel_Spawn(int e, gid_t16 gid)
{
	(void)gid;
	/* UTIL_PrecacheOther("monster_human_grunt") */
	PRECACHE(RESID(MON_HGRUNT_MDL));
	PRECACHE(RESID(MON_ROPE_SPR));
	ent_set_solid(e, SOLID_NOT);
	SetUse(e, FN_CHGruntRepel_RepelUse);
	return 0;
}

const game_class_t gc_MONSTER_GRUNT_REPEL = {
	.size = sizeof(CBaseEntity),
	.Spawn = CHGruntRepel_Spawn,
};

/* ---- monster_hgrunt_dead (CDeadHGrunt) ---------------------------------------------- */

static const char *const hg_poses[] = { "deadstomach", "deadside", "deadsitting" };

static int CDeadHGrunt_Spawn(int e, gid_t16 gid)
{
	CBaseMonster	*m = Mon_SpawnKeys(e, gid);
	int		pose = (int)KeyF(gid, K_POSE, 0), seq, body = ent_rnd(e)->body;

	PRECACHE(RESID(MON_HGRUNT_MDL));
	ent_set_model(e, RESID(MON_HGRUNT_MDL));
	ent_rnd(e)->effects = 0;
	ent_phys(e)->yaw_speed = 8;
	ent_rnd(e)->sequence = 0;
	m->m_bloodColor = BLOOD_COLOR_RED;
	if (pose < 0 || pose >= (int)ARRAYSIZE(hg_poses))
		pose = 0;
	seq = Mon_LookupSequence(e, hg_poses[pose]);
	if (seq < 0)
	{
		log_printf("Dead hgrunt with bad pose\n");
		seq = 0;
	}
	ent_rnd(e)->sequence = (uint16_t)seq;
	m->health = 8;
	/* map body 0-3: head grunt / commander, with / without gun */
	if (body >= 0 && body <= 3)
	{
		ent_rnd(e)->body = 0;
		ent_cv(e)->skin = 0;
		Mon_SetBodygroup(e, HEAD_GROUP, (body & 1) ? HEAD_COMMANDER : HEAD_GRUNT);
		Mon_SetBodygroup(e, GUN_GROUP, (body & 2) ? GUN_NONE : GUN_MP5);
	}
	Mon_MonsterInitDead(e);
	return 0;
}

static const game_vt_t deadhg_vt = {
	MON_GAME_VT,
	.Classify = Hg_Classify,
};

static const mon_vt_t deadhg_mvt = { .squad = 0 };

const game_class_t gc_MONSTER_HGRUNT_DEAD = {
	.size = sizeof(CBaseMonster),
	.Spawn = CDeadHGrunt_Spawn,
	.vt = &deadhg_vt,
	.ext = &deadhg_mvt,
	.save = &sv_CBaseMonster,
};
