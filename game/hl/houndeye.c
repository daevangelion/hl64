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
 * houndeye.c -- monster_houndeye (HLSDK dlls/houndeye.cpp CHoundeye), a
 * squad monster.
 */
#include "hl64/fx.h"
#include "game/hl/monster.h"

#define HOUNDEYE_MAX_ATTACK_RADIUS	384
#define HOUNDEYE_SQUAD_BONUS		1.1f
#define HOUNDEYE_EYE_FRAMES		4
#define HOUNDEYE_SOUND_STARTLE_VOLUME	128

enum {
	TASK_HOUND_CLOSE_EYE = LAST_COMMON_TASK + 1,
	TASK_HOUND_OPEN_EYE,
	TASK_HOUND_THREAT_DISPLAY,
	TASK_HOUND_FALL_ASLEEP,
	TASK_HOUND_WAKE_UP,
	TASK_HOUND_HOP_BACK
};

enum {
	SCHED_HOUND_AGITATED = LAST_COMMON_SCHEDULE + 1,
	SCHED_HOUND_HOP_RETREAT,
	SCHED_HOUND_FAIL,
};

#define HOUND_AE_WARN		1
#define HOUND_AE_STARTATTACK	2
#define HOUND_AE_THUMP		3
#define HOUND_AE_ANGERSOUND1	4
#define HOUND_AE_ANGERSOUND2	5
#define HOUND_AE_HOPBACK	6
#define HOUND_AE_CLOSE_EYE	7

/* nodes.h hint types the houndeye investigates */
#define HINT_WORLD_MACHINERY		4
#define HINT_WORLD_BLINKING_LIGHT	8
#define HINT_WORLD_HUMAN_BLOOD		10
#define HINT_WORLD_ALIEN_BLOOD		11

static const float sk_houndeyeHealth[3] = { 20, 20, 30 };
static const float sk_houndeyeDmgBlast[3] = { 10, 15, 15 };

typedef struct {
	CBASEENTITY_FIELDS
	CBASEDELAY_FIELDS
	CBASEANIMATING_FIELDS
	CBASETOGGLE_FIELDS
	CBASEMONSTER_FIELDS
	CSQUADMONSTER_FIELDS
	uint8_t		m_fAsleep;
	uint8_t		m_fDontBlink;
	uint16_t	he_pad;
	float		m_vecPackCenter[3];
} CHoundeye;

#define HOUND(e)	((CHoundeye *)ent_priv(e))

#define T(t, d)	{ t, (float)(d) }

static const Task_t tlHoundGuardPack[] = { T(TASK_STOP_MOVING, 0), T(TASK_GUARD, 0) };
static const Schedule_t slHoundGuardPack[] = {
	SCHED(tlHoundGuardPack, bits_COND_SEE_HATE | bits_COND_LIGHT_DAMAGE | bits_COND_HEAVY_DAMAGE |
		bits_COND_PROVOKED | bits_COND_HEAR_SOUND,
		bits_SOUND_COMBAT | bits_SOUND_WORLD | bits_SOUND_MEAT | bits_SOUND_PLAYER, "GuardPack")
};

static const Task_t tlHoundYell1[] = {
	T(TASK_STOP_MOVING, 0), T(TASK_FACE_IDEAL, 0), T(TASK_RANGE_ATTACK1, 0),
	T(TASK_SET_SCHEDULE, SCHED_HOUND_AGITATED),
};
static const Schedule_t slHoundRangeAttack[] = {
	SCHED(tlHoundYell1, bits_COND_LIGHT_DAMAGE | bits_COND_HEAVY_DAMAGE, 0, "HoundRangeAttack1"),
};

static const Task_t tlHoundSleep[] = {
	T(TASK_STOP_MOVING, 0), T(TASK_SET_ACTIVITY, ACT_IDLE), T(TASK_WAIT_RANDOM, 5),
	T(TASK_PLAY_SEQUENCE, ACT_CROUCH), T(TASK_SET_ACTIVITY, ACT_CROUCHIDLE),
	T(TASK_HOUND_FALL_ASLEEP, 0), T(TASK_WAIT_RANDOM, 25), T(TASK_HOUND_CLOSE_EYE, 0),
};
static const Schedule_t slHoundSleep[] = {
	SCHED(tlHoundSleep, bits_COND_HEAR_SOUND | bits_COND_LIGHT_DAMAGE | bits_COND_HEAVY_DAMAGE |
		bits_COND_NEW_ENEMY, bits_SOUND_COMBAT | bits_SOUND_PLAYER | bits_SOUND_WORLD, "Hound Sleep")
};

static const Task_t tlHoundWakeLazy[] = {
	T(TASK_STOP_MOVING, 0), T(TASK_HOUND_OPEN_EYE, 0), T(TASK_WAIT_RANDOM, 2.5),
	T(TASK_PLAY_SEQUENCE, ACT_STAND), T(TASK_HOUND_WAKE_UP, 0),
};
static const Schedule_t slHoundWakeLazy[] = { SCHED(tlHoundWakeLazy, 0, 0, "WakeLazy") };

static const Task_t tlHoundWakeUrgent[] = {
	T(TASK_HOUND_OPEN_EYE, 0), T(TASK_PLAY_SEQUENCE, ACT_HOP), T(TASK_FACE_IDEAL, 0),
	T(TASK_HOUND_WAKE_UP, 0),
};
static const Schedule_t slHoundWakeUrgent[] = { SCHED(tlHoundWakeUrgent, 0, 0, "WakeUrgent") };

static const Task_t tlHoundSpecialAttack1[] = {
	T(TASK_STOP_MOVING, 0), T(TASK_FACE_IDEAL, 0), T(TASK_SPECIAL_ATTACK1, 0),
	T(TASK_PLAY_SEQUENCE, ACT_IDLE_ANGRY),
};
static const Schedule_t slHoundSpecialAttack1[] = {
	SCHED(tlHoundSpecialAttack1, bits_COND_NEW_ENEMY | bits_COND_LIGHT_DAMAGE |
		bits_COND_HEAVY_DAMAGE | bits_COND_ENEMY_OCCLUDED, 0, "Hound Special Attack1")
};

static const Task_t tlHoundAgitated[] = { T(TASK_STOP_MOVING, 0), T(TASK_HOUND_THREAT_DISPLAY, 0) };
static const Schedule_t slHoundAgitated[] = {
	SCHED(tlHoundAgitated, bits_COND_NEW_ENEMY | bits_COND_LIGHT_DAMAGE | bits_COND_HEAVY_DAMAGE,
		0, "Hound Agitated")
};

static const Task_t tlHoundHopRetreat[] = {
	T(TASK_STOP_MOVING, 0), T(TASK_HOUND_HOP_BACK, 0),
	T(TASK_SET_SCHEDULE, SCHED_TAKE_COVER_FROM_ENEMY),
};
static const Schedule_t slHoundHopRetreat[] = { SCHED(tlHoundHopRetreat, 0, 0, "Hound Hop Retreat") };

static const Task_t tlHoundCombatFailPVS[] = {
	T(TASK_STOP_MOVING, 0), T(TASK_HOUND_THREAT_DISPLAY, 0), T(TASK_WAIT_FACE_ENEMY, 1),
};
static const Schedule_t slHoundCombatFailPVS[] = {
	SCHED(tlHoundCombatFailPVS, bits_COND_NEW_ENEMY | bits_COND_LIGHT_DAMAGE |
		bits_COND_HEAVY_DAMAGE, 0, "HoundCombatFailPVS")
};

static const Task_t tlHoundCombatFailNoPVS[] = {
	T(TASK_STOP_MOVING, 0), T(TASK_HOUND_THREAT_DISPLAY, 0), T(TASK_WAIT_FACE_ENEMY, 2),
	T(TASK_SET_ACTIVITY, ACT_IDLE), T(TASK_WAIT_PVS, 0),
};
static const Schedule_t slHoundCombatFailNoPVS[] = {
	SCHED(tlHoundCombatFailNoPVS, bits_COND_NEW_ENEMY | bits_COND_LIGHT_DAMAGE |
		bits_COND_HEAVY_DAMAGE, 0, "HoundCombatFailNoPVS")
};

static const int16_t heIdle[] = { R_HE_IDLE1, R_HE_IDLE2, R_HE_IDLE3 };
static const int16_t heWarmUp[] = { R_HE_ATTACK1, R_HE_ATTACK3 };
static const int16_t heHunt[] = { R_HE_HUNT1, R_HE_HUNT2, R_HE_HUNT3 };
static const int16_t heAlert[] = { R_HE_ALERT1, R_HE_ALERT2, R_HE_ALERT3 };
static const int16_t heDie[] = { R_HE_DIE1, R_HE_DIE2, R_HE_DIE3 };
static const int16_t hePain[] = { R_HE_PAIN3, R_HE_PAIN4, R_HE_PAIN5 };
static const int16_t heBlast[] = { R_HE_BLAST1, R_HE_BLAST2, R_HE_BLAST3 };

static int He_Classify(int e) { (void)e; return CLASS_ALIEN_MONSTER; }

static int He_FValidateHintType(int e, int hint)
{
	(void)e;
	return hint == HINT_WORLD_MACHINERY || hint == HINT_WORLD_BLINKING_LIGHT ||
		hint == HINT_WORLD_HUMAN_BLOOD || hint == HINT_WORLD_ALIEN_BLOOD;
}

static int He_FCanActiveIdle(int e)
{
	int leader, i, mem;

	if (!Sqm_InSquad(e))
		return 1;
	leader = Sqm_MySquadLeader(e);
	for (i = 0; i < MAX_SQUAD_MEMBERS; i++)
	{
		mem = Sqm_MySquadMember(leader, i);
		if (mem >= 0 && mem != e && SQM(mem)->m_iHintNode != NO_NODE)
			return 0;
	}
	return 1;
}

static int He_CheckRangeAttack1(int e, float dot, float dist)
{
	(void)e;
	return dist <= HOUNDEYE_MAX_ATTACK_RADIUS * 0.5f && dot >= 0.3f;
}

static void He_SetYawSpeed(int e)
{
	int ys = 90;

	switch (MON(e)->m_Activity)
	{
	case ACT_CROUCHIDLE: ys = 0; break;
	case ACT_IDLE: ys = 60; break;
	default: break;
	}
	ent_phys(e)->yaw_speed = (float)ys;
}

static void He_SetActivity(int e, int act)
{
	CBaseMonster	*m = MON(e);
	int		seq;

	if (act == m->m_Activity)
		return;
	if (m->m_MonsterState == MONSTERSTATE_COMBAT && act == ACT_IDLE && RANDOM_LONG(0, 1))
	{
		seq = Mon_LookupSequence(e, "madidle");
		m->m_Activity = act;
		m->m_IdealActivity = act;
		if (seq > ACTIVITY_NOT_AVAILABLE)
		{
			ent_rnd(e)->sequence = seq;
			ent_rnd(e)->frame = 0;
			Mon_ResetSequenceInfo(e);
			He_SetYawSpeed(e);
		}
	}
	else
		CBaseMonster_SetActivity(e, act);
}

static void He_IdleSound(int e) { EMIT_SOUND(e, CHAN_VOICE, RANDOM_SOUND_ARRAY(heIdle), 1, ATTN_NORM); }
static void He_WarmUpSound(int e) { EMIT_SOUND(e, CHAN_WEAPON, RANDOM_SOUND_ARRAY(heWarmUp), 0.7f, ATTN_NORM); }
static void He_WarnSound(int e) { EMIT_SOUND(e, CHAN_VOICE, RANDOM_SOUND_ARRAY(heHunt), 1, ATTN_NORM); }
static void He_DeathSound(int e) { EMIT_SOUND(e, CHAN_VOICE, RANDOM_SOUND_ARRAY(heDie), 1, ATTN_NORM); }
static void He_PainSound(int e) { EMIT_SOUND(e, CHAN_VOICE, RANDOM_SOUND_ARRAY(hePain), 1, ATTN_NORM); }

static void He_AlertSound(int e)
{
	if (Sqm_InSquad(e) && !Sqm_IsLeader(e))
		return;
	EMIT_SOUND(e, CHAN_VOICE, RANDOM_SOUND_ARRAY(heAlert), 1, ATTN_NORM);
}

/* WriteBeamColor */
static void beam_color(int e, int rgb[3])
{
	int n = Sqm_InSquad(e) ? Sqm_SquadCount(e) : 0;

	switch (n)
	{
	case 2: rgb[0] = 101; rgb[1] = 133; rgb[2] = 221; break;
	case 3: rgb[0] = 67; rgb[1] = 85; rgb[2] = 255; break;
	case 4: rgb[0] = 62; rgb[1] = 33; rgb[2] = 211; break;
	default: rgb[0] = 188; rgb[1] = 220; rgb[2] = 255; break;
	}
}

static void He_SonicAttack(int e)
{
	float	org[3], c[3], dmg, dist;
	int	rgb[3], r, n;

	EMIT_SOUND(e, CHAN_WEAPON, RANDOM_SOUND_ARRAY(heBlast), 1, ATTN_NORM);
	VecCopy(ent_cv(e)->origin, org);
	org[2] += 16;
	beam_color(e, rgb);
	/* the two rings: HLSDK's ring end height (radius / 0.2), life 2 = 0.2 s */
	fx_beam_cylinder(org, HOUNDEYE_MAX_ATTACK_RADIUS / 0.2f, RESID(MON_SHOCKWAVE_SPR), 2, 16, 0,
		rgb[0], rgb[1], rgb[2], 255);
	fx_beam_cylinder(org, (HOUNDEYE_MAX_ATTACK_RADIUS / 2) / 0.2f, RESID(MON_SHOCKWAVE_SPR), 2,
		16, 0, rgb[0], rgb[1], rgb[2], 255);
	r = -1;
	while ((r = UTIL_FindEntityInSphere(r, ent_cv(e)->origin, HOUNDEYE_MAX_ATTACK_RADIUS)) >= 0)
	{
		if (!ent_priv(r) || CBase(r)->takedamage == DAMAGE_NO)
			continue;
		if (ent_idc(r)->cls == CLS_MONSTER_HOUNDEYE)
			continue;
		n = Sqm_SquadCount(e);
		dmg = mon_skill(sk_houndeyeDmgBlast);
		if (n > 1)
			dmg = dmg + dmg * (HOUNDEYE_SQUAD_BONUS * (float)(n - 1));
		Center(r, c);
		dist = mv_dist(c, ent_cv(e)->origin);
		dmg -= (dist / HOUNDEYE_MAX_ATTACK_RADIUS) * dmg;
		if (!FVisible(e, r))
		{
			if (IsPlayer(r))
				dmg *= 0.5f;
			else if (ent_idc(r)->cls != CLS_FUNC_BREAKABLE && ent_idc(r)->cls != CLS_FUNC_PUSHABLE)
				dmg = 0;
		}
		if (dmg > 0)
			TakeDamage(r, e, e, dmg, DMG_SONIC | DMG_ALWAYSGIB);
	}
}

static void He_HandleAnimEvent(int e, const MonsterEvent_t *ev)
{
	CHoundeye	*h = HOUND(e);
	float		v[3];

	switch (ev->event)
	{
	case HOUND_AE_WARN:
		He_WarnSound(e);
		break;
	case HOUND_AE_STARTATTACK:
		He_WarmUpSound(e);
		break;
	case HOUND_AE_HOPBACK:
		EFlagsClear(e, FL_ONGROUND);
		VecScale(gpGlobals.v_forward, -200, v);
		v[2] += (0.6f * settings.gravity) * 0.5f;
		ent_set_velocity(e, v);
		break;
	case HOUND_AE_THUMP:
		He_SonicAttack(e);
		break;
	case HOUND_AE_ANGERSOUND1:
		EMIT_SOUND(e, CHAN_VOICE, RESID(HE_PAIN3), 1, ATTN_NORM);
		break;
	case HOUND_AE_ANGERSOUND2:
		EMIT_SOUND(e, CHAN_VOICE, RESID(HE_PAIN1), 1, ATTN_NORM);
		break;
	case HOUND_AE_CLOSE_EYE:
		if (!h->m_fDontBlink)
			ent_cv(e)->skin = HOUNDEYE_EYE_FRAMES - 1;
		break;
	default:
		CBaseMonster_HandleAnimEvent(e, ev);
	}
}

static void He_StartTask(int e, const Task_t *t)
{
	CHoundeye *h = HOUND(e);

	h->m_iTaskStatus = TASKSTATUS_RUNNING;
	switch (t->iTask)
	{
	case TASK_HOUND_FALL_ASLEEP:
		h->m_fAsleep = 1;
		h->m_iTaskStatus = TASKSTATUS_COMPLETE;
		break;
	case TASK_HOUND_WAKE_UP:
		h->m_fAsleep = 0;
		h->m_iTaskStatus = TASKSTATUS_COMPLETE;
		break;
	case TASK_HOUND_OPEN_EYE:
		h->m_fDontBlink = 0;
		h->m_iTaskStatus = TASKSTATUS_COMPLETE;
		break;
	case TASK_HOUND_CLOSE_EYE:
		ent_cv(e)->skin = 0;
		h->m_fDontBlink = 1;
		break;
	case TASK_HOUND_THREAT_DISPLAY:
		h->m_IdealActivity = ACT_IDLE_ANGRY;
		break;
	case TASK_HOUND_HOP_BACK:
		h->m_IdealActivity = ACT_LEAP;
		break;
	case TASK_RANGE_ATTACK1:
		h->m_IdealActivity = ACT_RANGE_ATTACK1;
		break;
	case TASK_SPECIAL_ATTACK1:
		h->m_IdealActivity = ACT_SPECIAL_ATTACK1;
		break;
	case TASK_GUARD:
		h->m_IdealActivity = ACT_GUARD;
		break;
	default:
		CBaseMonster_StartTask(e, t);
	}
}

static void He_RunTask(int e, const Task_t *t)
{
	CHoundeye	*h = HOUND(e);
	ent_rnd_t	*rn = ent_rnd(e);
	float		life, org[3];

	switch (t->iTask)
	{
	case TASK_HOUND_THREAT_DISPLAY:
		Mon_MakeIdealYaw(e, h->m_vecEnemyLKP);
		Mon_ChangeYaw(e, (int)ent_phys(e)->yaw_speed);
		if (h->anim.m_fSequenceFinished)
			Mon_TaskComplete((CBaseMonster *)h);
		break;
	case TASK_HOUND_CLOSE_EYE:
		if (ent_cv(e)->skin < HOUNDEYE_EYE_FRAMES - 1)
			ent_cv(e)->skin++;
		break;
	case TASK_HOUND_HOP_BACK:
		if (h->anim.m_fSequenceFinished)
			Mon_TaskComplete((CBaseMonster *)h);
		break;
	case TASK_SPECIAL_ATTACK1:
		ent_cv(e)->skin = (int16_t)RANDOM_LONG(0, HOUNDEYE_EYE_FRAMES - 1);
		Mon_MakeIdealYaw(e, h->m_vecEnemyLKP);
		Mon_ChangeYaw(e, (int)ent_phys(e)->yaw_speed);
		life = rn->framerate * h->anim.m_flFrameRate;
		life = life != 0 ? (255 - rn->frame) / life : 0.1f;
		if (life < 0.1f)
			life = 0.1f;
		VecCopy(ent_cv(e)->origin, org);
		org[2] += 16;
		fx_implosion(org, (int)(50 * life + 100), (int)(rn->frame / 25.0f), (int)(life * 10));
		if (h->anim.m_fSequenceFinished)
		{
			He_SonicAttack(e);
			Mon_TaskComplete((CBaseMonster *)h);
		}
		break;
	default:
		CBaseMonster_RunTask(e, t);
	}
}

static void He_PrescheduleThink(int e)
{
	CHoundeye	*h = HOUND(e);
	int16_t		*skin = &ent_cv(e)->skin;
	int		i, mem, n = 0;

	if (h->m_MonsterState == MONSTERSTATE_COMBAT && h->m_Activity == ACT_RUN &&
		RANDOM_FLOAT(0, 1) < 0.2f)
		He_WarnSound(e);
	if (!h->m_fDontBlink)
	{
		if (*skin == 0 && RANDOM_LONG(0, 0x7F) == 0)
			*skin = HOUNDEYE_EYE_FRAMES - 1;
		else if (*skin != 0)
			(*skin)--;
	}
	if (Sqm_IsLeader(e))
	{
		for (i = 0; i < MAX_SQUAD_MEMBERS; i++)
		{
			mem = Sqm_MySquadMember(e, i);
			if (mem >= 0)
			{
				n++;
				VecAdd(h->m_vecPackCenter, ent_cv(mem)->origin, h->m_vecPackCenter);
			}
		}
		VecScale(h->m_vecPackCenter, 1.0f / (float)n, h->m_vecPackCenter);
	}
}

static const Schedule_t *He_GetScheduleOfType(int e, int type)
{
	CHoundeye	*h = HOUND(e);
	CSound		*snd;

	if (h->m_fAsleep)
	{
		if (Mon_HasConditions((CBaseMonster *)h, bits_COND_HEAR_SOUND))
		{
			snd = Mon_PBestSound(e);
			if (snd)
			{
				Mon_MakeIdealYaw(e, snd->m_vecOrigin);
				if (Mon_FLSoundVolume(e, snd) >= HOUNDEYE_SOUND_STARTLE_VOLUME)
					return &slHoundWakeUrgent[0];
			}
			return &slHoundWakeLazy[0];
		}
		if (Mon_HasConditions((CBaseMonster *)h, bits_COND_NEW_ENEMY))
			return &slHoundWakeUrgent[0];
		return &slHoundWakeLazy[0];
	}
	switch (type)
	{
	case SCHED_IDLE_STAND:
		if (Sqm_InSquad(e) && !Sqm_IsLeader(e) && !h->m_fAsleep && RANDOM_LONG(0, 29) < 1)
			return &slHoundSleep[0];
		break;
	case SCHED_RANGE_ATTACK1:
		return &slHoundRangeAttack[0];
	case SCHED_SPECIAL_ATTACK1:
		return &slHoundSpecialAttack1[0];
	case SCHED_GUARD:
		return &slHoundGuardPack[0];
	case SCHED_HOUND_AGITATED:
		return &slHoundAgitated[0];
	case SCHED_HOUND_HOP_RETREAT:
		return &slHoundHopRetreat[0];
	case SCHED_FAIL:
		if (h->m_MonsterState == MONSTERSTATE_COMBAT)
			return ent_client_in_pvs(e) >= 0 ? &slHoundCombatFailPVS[0] : &slHoundCombatFailNoPVS[0];
		break;
	}
	return CSquadMonster_GetScheduleOfType(e, type);
}

static const Schedule_t *He_GetSchedule(int e)
{
	CHoundeye	*h = HOUND(e);
	col_trace_t	tr;
	float		end[3];

	if (h->m_MonsterState == MONSTERSTATE_COMBAT)
	{
		if (Mon_HasConditions((CBaseMonster *)h, bits_COND_ENEMY_DEAD))
			return CBaseMonster_GetSchedule(e);
		if (Mon_HasConditions((CBaseMonster *)h, bits_COND_LIGHT_DAMAGE | bits_COND_HEAVY_DAMAGE))
		{
			if (RANDOM_FLOAT(0, 1) <= 0.4f)
			{
				UTIL_MakeVectors(ent_cv(e)->angles);
				VecMA(ent_cv(e)->origin, -128, gpGlobals.v_forward, end);
				UTIL_TraceHull(ent_cv(e)->origin, end, dont_ignore_monsters, head_hull, e, &tr);
				if (tr.frac == 1.0f)
					return Mon_GetScheduleOfType(e, SCHED_HOUND_HOP_RETREAT);
			}
			return Mon_GetScheduleOfType(e, SCHED_TAKE_COVER_FROM_ENEMY);
		}
		if (Mon_HasConditions((CBaseMonster *)h, bits_COND_CAN_RANGE_ATTACK1))
		{
			if (Sqm_OccupySlot(e, bits_SLOTS_HOUND_ATTACK))
				return Mon_GetScheduleOfType(e, SCHED_RANGE_ATTACK1);
			return Mon_GetScheduleOfType(e, SCHED_HOUND_AGITATED);
		}
	}
	return CBaseMonster_GetSchedule(e);
}

static int CHoundeye_Spawn(int e, gid_t16 gid)
{
	CBaseMonster	*m = Mon_SpawnKeys(e, gid);
	CHoundeye	*h = (CHoundeye *)m;
	static const float mn[3] = { -16, -16, 0 }, mx[3] = { 16, 16, 36 };
	static const int16_t pre[] = {
		R_HE_ALERT1, R_HE_ALERT2, R_HE_ALERT3, R_HE_DIE1, R_HE_DIE2, R_HE_DIE3,
		R_HE_IDLE1, R_HE_IDLE2, R_HE_IDLE3, R_HE_HUNT1, R_HE_HUNT2, R_HE_HUNT3,
		R_HE_PAIN1, R_HE_PAIN3, R_HE_PAIN4, R_HE_PAIN5, R_HE_ATTACK1, R_HE_ATTACK3,
		R_HE_BLAST1, R_HE_BLAST2, R_HE_BLAST3, R_MON_SHOCKWAVE_SPR,
	};

	Sqm_Init(e);
	PRECACHE(RESID(MON_HOUNDEYE_MDL));
	PRECACHE_SOUND_ARRAY(pre);
	ent_set_model(e, RESID(MON_HOUNDEYE_MDL));
	ent_set_size(e, mn, mx);
	ent_set_solid(e, SOLID_SLIDEBOX);
	ent_set_movetype(e, MOVETYPE_STEP);
	m->m_bloodColor = BLOOD_COLOR_YELLOW;
	ent_rnd(e)->effects = 0;
	m->health = mon_skill(sk_houndeyeHealth);
	ent_phys(e)->yaw_speed = 5;
	m->m_flFieldOfView = 0.5f;
	m->m_MonsterState = MONSTERSTATE_NONE;
	h->m_fAsleep = 0;
	h->m_fDontBlink = 0;
	m->m_afCapability |= bits_CAP_SQUAD;
	Mon_MonsterInit(e);
	return 0;
}

static const game_vt_t he_vt = {
	MON_GAME_VT_BASE,
	.TraceAttack = CBaseMonster_TraceAttack,
	.TakeDamage = CBaseMonster_TakeDamage,
	.IRelationship = CBaseMonster_IRelationship,
	.Killed = CSquadMonster_Killed,
	.Classify = He_Classify,
};

static const mon_vt_t he_mvt = {
	.squad = 1,
	.SetYawSpeed = He_SetYawSpeed,
	.SetActivity = He_SetActivity,
	.HandleAnimEvent = He_HandleAnimEvent,
	.StartTask = He_StartTask,
	.RunTask = He_RunTask,
	.PrescheduleThink = He_PrescheduleThink,
	.CheckRangeAttack1 = He_CheckRangeAttack1,
	.FValidateHintType = He_FValidateHintType,
	.FCanActiveIdle = He_FCanActiveIdle,
	.GetScheduleOfType = He_GetScheduleOfType,
	.GetSchedule = He_GetSchedule,
	.IdleSound = He_IdleSound,
	.AlertSound = He_AlertSound,
	.DeathSound = He_DeathSound,
	.PainSound = He_PainSound,
	.CheckEnemy = CSquadMonster_CheckEnemy,
	.StartMonster = CSquadMonster_StartMonster,
	.ScheduleChange = CSquadMonster_ScheduleChange,
	.FValidateCover = CSquadMonster_FValidateCover,
	.GetIdealState = CSquadMonster_GetIdealState,
};

const game_class_t gc_MONSTER_HOUNDEYE = {
	.size = sizeof(CHoundeye),
	.Spawn = CHoundeye_Spawn,
	.vt = &he_vt,
	.ext = &he_mvt,
};
