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
 * talkmonster.c -- CTalkMonster (HLSDK dlls/talkmonster.cpp): the talking
 * monsters' (scientist, barney) speech, following and their schedules.
 * Sentence groups are res.def GROUP ids in m_szGrp (set by each class's
 * TalkInit); "!NAME" sentences are SENTENCE res.
 */
#include <math.h>

#include "game/hl/monster.h"

#define VOL_NORM	1.0f

float	g_talkWaitTime;

/* m_szFriends: monster_barney, monster_scientist (monster_sitting_scientist
 * is not a class of these maps) */
static cls_t m_szFriends[TLK_CFRIENDS] = { CLS_MONSTER_BARNEY, CLS_MONSTER_SCIENTIST, CLS_NONE };

void Tlk_SetFriends(cls_t a, cls_t b, cls_t c)
{
	m_szFriends[0] = a;
	m_szFriends[1] = b;
	m_szFriends[2] = c;
}

#define T(t, d)		{ t, (float)(d) }

static const Task_t tlIdleResponse[] = {
	T(TASK_SET_ACTIVITY, ACT_IDLE), T(TASK_WAIT, 0.5), T(TASK_TLK_EYECONTACT, 0),
	T(TASK_TLK_RESPOND, 0), T(TASK_TLK_IDEALYAW, 0), T(TASK_FACE_IDEAL, 0),
	T(TASK_SET_ACTIVITY, ACT_SIGNAL3), T(TASK_TLK_EYECONTACT, 0),
};
const Schedule_t slIdleResponse[] = {
	SCHED(tlIdleResponse, bits_COND_NEW_ENEMY | bits_COND_LIGHT_DAMAGE | bits_COND_HEAVY_DAMAGE, 0,
		"Idle Response")
};

static const Task_t tlIdleSpeak[] = {
	T(TASK_TLK_SPEAK, 0), T(TASK_TLK_IDEALYAW, 0), T(TASK_FACE_IDEAL, 0),
	T(TASK_SET_ACTIVITY, ACT_SIGNAL3), T(TASK_TLK_EYECONTACT, 0), T(TASK_WAIT_RANDOM, 0.5),
};
const Schedule_t slIdleSpeak[] = {
	SCHED(tlIdleSpeak, bits_COND_NEW_ENEMY | bits_COND_CLIENT_PUSH | bits_COND_LIGHT_DAMAGE |
		bits_COND_HEAVY_DAMAGE, 0, "Idle Speak")
};

static const Task_t tlIdleSpeakWait[] = {
	T(TASK_SET_ACTIVITY, ACT_SIGNAL3), T(TASK_TLK_SPEAK, 0), T(TASK_TLK_EYECONTACT, 0),
	T(TASK_WAIT, 2),
};
const Schedule_t slIdleSpeakWait[] = {
	SCHED(tlIdleSpeakWait, bits_COND_NEW_ENEMY | bits_COND_CLIENT_PUSH | bits_COND_LIGHT_DAMAGE |
		bits_COND_HEAVY_DAMAGE, 0, "Idle Speak Wait")
};

static const Task_t tlIdleHello[] = {
	T(TASK_SET_ACTIVITY, ACT_SIGNAL3),
	T(TASK_TLK_HELLO, 0), T(TASK_TLK_EYECONTACT, 0), T(TASK_WAIT, 0.5),
	T(TASK_TLK_HELLO, 0), T(TASK_TLK_EYECONTACT, 0), T(TASK_WAIT, 0.5),
	T(TASK_TLK_HELLO, 0), T(TASK_TLK_EYECONTACT, 0), T(TASK_WAIT, 0.5),
	T(TASK_TLK_HELLO, 0), T(TASK_TLK_EYECONTACT, 0), T(TASK_WAIT, 0.5),
};
const Schedule_t slIdleHello[] = {
	SCHED(tlIdleHello, bits_COND_NEW_ENEMY | bits_COND_CLIENT_PUSH | bits_COND_LIGHT_DAMAGE |
		bits_COND_HEAVY_DAMAGE | bits_COND_HEAR_SOUND | bits_COND_PROVOKED, bits_SOUND_COMBAT,
		"Idle Hello")
};

static const Task_t tlIdleStopShooting[] = { T(TASK_TLK_STOPSHOOTING, 0) };
const Schedule_t slIdleStopShooting[] = {
	SCHED(tlIdleStopShooting, bits_COND_NEW_ENEMY | bits_COND_LIGHT_DAMAGE | bits_COND_HEAVY_DAMAGE |
		bits_COND_HEAR_SOUND, 0, "Idle Stop Shooting")
};

static const Task_t tlMoveAway[] = {
	T(TASK_SET_FAIL_SCHEDULE, SCHED_MOVE_AWAY_FAIL), T(TASK_STORE_LASTPOSITION, 0),
	T(TASK_MOVE_AWAY_PATH, 100), T(TASK_WALK_PATH_FOR_UNITS, 100), T(TASK_STOP_MOVING, 0),
	T(TASK_FACE_PLAYER, 0.5),
};
const Schedule_t slMoveAway[] = { SCHED(tlMoveAway, 0, 0, "MoveAway") };

static const Task_t tlMoveAwayFail[] = { T(TASK_STOP_MOVING, 0), T(TASK_FACE_PLAYER, 0.5) };
const Schedule_t slMoveAwayFail[] = { SCHED(tlMoveAwayFail, 0, 0, "MoveAwayFail") };

static const Task_t tlMoveAwayFollow[] = {
	T(TASK_SET_FAIL_SCHEDULE, SCHED_TARGET_FACE), T(TASK_STORE_LASTPOSITION, 0),
	T(TASK_MOVE_AWAY_PATH, 100), T(TASK_WALK_PATH_FOR_UNITS, 100), T(TASK_STOP_MOVING, 0),
	T(TASK_SET_SCHEDULE, SCHED_TARGET_FACE),
};
const Schedule_t slMoveAwayFollow[] = { SCHED(tlMoveAwayFollow, 0, 0, "MoveAwayFollow") };

static const Task_t tlTlkIdleWatchClient[] = {
	T(TASK_STOP_MOVING, 0), T(TASK_SET_ACTIVITY, ACT_IDLE), T(TASK_TLK_LOOK_AT_CLIENT, 6),
};
static const Task_t tlTlkIdleWatchClientStare[] = {
	T(TASK_STOP_MOVING, 0), T(TASK_SET_ACTIVITY, ACT_IDLE), T(TASK_TLK_CLIENT_STARE, 6),
	T(TASK_TLK_STARE, 0), T(TASK_TLK_IDEALYAW, 0), T(TASK_FACE_IDEAL, 0),
	T(TASK_SET_ACTIVITY, ACT_SIGNAL3), T(TASK_TLK_EYECONTACT, 0),
};
#define COND_WATCH (bits_COND_NEW_ENEMY | bits_COND_LIGHT_DAMAGE | bits_COND_HEAVY_DAMAGE | \
	bits_COND_HEAR_SOUND | bits_COND_SMELL | bits_COND_CLIENT_PUSH | bits_COND_CLIENT_UNSEEN | \
	bits_COND_PROVOKED)
#define SND_WATCH (bits_SOUND_COMBAT | bits_SOUND_DANGER | bits_SOUND_MEAT | bits_SOUND_CARCASS | \
	bits_SOUND_GARBAGE)
const Schedule_t slTlkIdleWatchClient[] = {
	SCHED(tlTlkIdleWatchClient, COND_WATCH, SND_WATCH, "TlkIdleWatchClient"),
	SCHED(tlTlkIdleWatchClientStare, COND_WATCH, SND_WATCH, "TlkIdleWatchClientStare"),
};

static const Task_t tlTlkIdleEyecontact[] = {
	T(TASK_TLK_IDEALYAW, 0), T(TASK_FACE_IDEAL, 0), T(TASK_SET_ACTIVITY, ACT_SIGNAL3),
	T(TASK_TLK_EYECONTACT, 0),
};
const Schedule_t slTlkIdleEyecontact[] = {
	SCHED(tlTlkIdleEyecontact, bits_COND_NEW_ENEMY | bits_COND_CLIENT_PUSH | bits_COND_LIGHT_DAMAGE |
		bits_COND_HEAVY_DAMAGE, 0, "TlkIdleEyecontact")
};

/* ---- helpers ------------------------------------------------------------------ */

float UTIL_DotPoints(const float src[3], const float check[3], const float dir[3])
{
	float los[3], d2[3];

	VecSub(check, src, los);
	mv_norm2d(los, los);
	VecSet(d2, dir[0], dir[1], 0);
	return los[0] * d2[0] + los[1] * d2[1];
}

void Mon_SentenceStop(int e) { snd_stop(e, CHAN_VOICE); }
void Tlk_StopTalking(int e) { Mon_SentenceStop(e); }

static float ent_hp(int e) { return ent_priv(e) ? CBase(e)->health : 0; }
static float ent_maxhp(int e) { return ent_priv(e) ? CBase(e)->max_health : 0; }

int Tlk_IsTalking(int e) { return TALK(e)->m_flStopTalkTime > gtime(); }

int Tlk_IsFollowing(int e)
{
	int t = EHGet(TALK(e)->m_hTargetEnt);
	return t >= 0 && IsPlayer(t);
}

static int friend_number(int e, int i)
{
	return mon_vt(e)->FriendNumber ? mon_vt(e)->FriendNumber(e, i) : i;
}

void CTalkMonster_SetActivity(int e, int act)
{
	if (act == ACT_IDLE && Tlk_IsTalking(e))
		act = ACT_SIGNAL3;
	if (act == ACT_SIGNAL3 && Mon_LookupActivity(e, ACT_SIGNAL3) == ACTIVITY_NOT_AVAILABLE)
		act = ACT_IDLE;
	CBaseMonster_SetActivity(e, act);
}

int Tlk_GetVoicePitch(int e) { return TALK(e)->m_voicePitch + RANDOM_LONG(0, 3); }

void Tlk_Talk(int e, float d)
{
	TALK(e)->m_flStopTalkTime = gtime() + (d <= 0 ? 3 : d);
}

void CTalkMonster_PlaySentence(int e, res_t s, float duration, float volume, float attn)
{
	CTalkMonster *t = TALK(e);

	if (!s)
		return;
	Tlk_Talk(e, duration);
	g_talkWaitTime = gtime() + duration + 2.0f;
	if (RES_KIND(s) == RES_K_SENTENCE)
		snd_sentence(e, CHAN_VOICE, sentence_id(s), volume, attn, 0, Tlk_GetVoicePitch(e));
	else
		SENTENCEG_PlayRndSz(e, s, volume, attn, 0, Tlk_GetVoicePitch(e));
	t->m_bitsSaid |= bit_saidHelloPlayer;
}

void CTalkMonster_PlayScriptedSentence(int e, res_t s, float duration, float volume, float attn,
	int bConcurrent, int listener)
{
	CTalkMonster *t = TALK(e);

	if (!bConcurrent)
		Tlk_ShutUpFriends(e);
	Mon_ClearConditions((CBaseMonster *)t, bits_COND_CLIENT_PUSH);
	t->m_useTime = gtime() + duration;
	Mon_PlaySentence(e, s, duration, volume, attn);
	t->m_hTalkTarget = EHANDLE(listener);
}

/* ---- friends ------------------------------------------------------------------- */

int Tlk_EnumFriends(int e, int prev, int listNumber, int bTrace)
{
	cls_t		cls = m_szFriends[friend_number(e, listNumber)];
	int		f = prev;
	col_trace_t	tr;
	float		chk[3], dl[3];

	if (cls == CLS_NONE)
		return -1;
	while ((f = ent_next_class(f, cls, -1)) >= 0)
	{
		if (f == e || !IsAlive(f))
			continue;
		if (bTrace)
		{
			/* sight is per layer (FVisible): another layer's friend is never
			 * seen, whatever a trace to its far-off point would report */
			if (LayerDelta(f, e, dl))
				continue;
			VecCopy(ent_cv(f)->origin, chk);
			chk[2] = ent_cv(f)->absmax[2];
			UTIL_TraceLine(ent_cv(e)->origin, chk, ignore_monsters, e, &tr);
		}
		else
			tr.frac = 1.0f;
		if (tr.frac == 1.0f)
			return f;
	}
	return -1;
}

void Tlk_AlertFriends(int e)
{
	int i, f;

	for (i = 0; i < TLK_CFRIENDS; i++)
	{
		f = -1;
		while ((f = Tlk_EnumFriends(e, f, i, 1)) >= 0)
			if (IsAlive(f))
				MON(f)->m_afMemory |= bits_MEMORY_PROVOKED;
	}
}

void Tlk_ShutUpFriends(int e)
{
	int i, f;

	for (i = 0; i < TLK_CFRIENDS; i++)
	{
		f = -1;
		while ((f = Tlk_EnumFriends(e, f, i, 1)) >= 0)
			Mon_SentenceStop(f);
	}
}

void Tlk_LimitFollowers(int e, int player, int maxFollowers)
{
	int i, f, count = 0;

	for (i = 0; i < TLK_CFRIENDS; i++)
	{
		f = -1;
		while ((f = Tlk_EnumFriends(e, f, i, 0)) >= 0)
		{
			if (EHGet(MON(f)->m_hTargetEnt) == player)
			{
				count++;
				if (count > maxFollowers)
					Mon_StopFollowing(f, 1);
			}
		}
	}
}

float Tlk_TargetDistance(int e)
{
	int t = EHGet(TALK(e)->m_hTargetEnt);

	if (t < 0 || !IsAlive(t))
		return 1e6f;
	{
		float o[3];

		OriginIn(t, e, o);
		return mv_dist(o, ent_cv(e)->origin);
	}
}

int Tlk_FindNearestFriend(int e, int fPlayer)
{
	int		i, f, nearest = -1, cfriends = fPlayer ? 1 : TLK_CFRIENDS;
	float		range = 10000000.0f, start[3], chk[3], dl[3], d;
	col_trace_t	tr;
	cls_t		cls;

	VecCopy(ent_cv(e)->origin, start);
	start[2] = ent_cv(e)->absmax[2];
	for (i = cfriends - 1; i > -1; i--)
	{
		cls = fPlayer ? CLS_PLAYER : m_szFriends[friend_number(e, i)];
		if (cls == CLS_NONE)
			continue;
		f = -1;
		for (;;)
		{
			if (cls == CLS_PLAYER)
				f = (f < 0 && ent_is_live(ENT_PLAYER)) ? ENT_PLAYER : -1;
			else
				f = ent_next_class(f, cls, -1);
			if (f < 0)
				break;
			if (f == e || !IsAlive(f))
				continue;
			if (!mon_is_monster(f))
				continue;
			if (f != ENT_PLAYER && (MON(f)->m_MonsterState == MONSTERSTATE_SCRIPT ||
				MON(f)->m_MonsterState == MONSTERSTATE_PRONE))
				continue;
			OriginIn(f, e, chk);
			chk[2] = ent_cv(f)->absmax[2];
			if (LayerDelta(f, e, dl))
				chk[2] += dl[2];
			d = mv_dist(start, chk);
			if (range > d)
			{
				UTIL_TraceLine(start, chk, ignore_monsters, e, &tr);
				if (tr.frac == 1.0f && d < TALKRANGE_MIN)
				{
					nearest = f;
					range = d;
				}
			}
		}
	}
	return nearest;
}

void Tlk_IdleHeadTurn(int e, const float fr[3])
{
	float d[3], yaw;

	if (MON(e)->m_afCapability & bits_CAP_TURN_HEAD)
	{
		VecSub(fr, ent_cv(e)->origin, d);
		yaw = Mon_VecToYaw(e, d) - ent_cv(e)->angles[1];
		if (yaw > 180)
			yaw -= 360;
		if (yaw < -180)
			yaw += 360;
		Mon_SetBoneController(e, 0, yaw);
	}
}

/* ---- speech decisions ------------------------------------------------------------- */

int Tlk_FOkToSpeak(int e)
{
	CTalkMonster	*t = TALK(e);
	int		en;

	if (t->m_MonsterState == MONSTERSTATE_PRONE || t->m_IdealMonsterState == MONSTERSTATE_PRONE)
		return 0;
	if (t->deadflag != DEAD_NO)
		return 0;
	if (gtime() <= g_talkWaitTime)
		return 0;
	if (t->spawnflags & SF_MONSTER_GAG)
		return 0;
	if (!IsAlive(e) || ent_client_in_pvs(e) <= 0)
		return 0;
	en = EHGet(t->m_hEnemy);
	if (en >= 0 && FVisible(e, en))
		return 0;
	return 1;
}

int CTalkMonster_CanPlaySentence(int e, int fDisregardState)
{
	if (fDisregardState)
		return Mon_IsAllowedToSpeak(e);
	return Tlk_FOkToSpeak(e);
}

void Tlk_IdleRespond(int e)
{
	Mon_PlaySentence(e, TALK(e)->m_szGrp[TLK_ANSWER], RANDOM_FLOAT(2.8f, 3.2f), VOL_NORM, ATTN_IDLE);
}

int Tlk_FIdleStare(int e)
{
	if (!Tlk_FOkToSpeak(e))
		return 0;
	Mon_PlaySentence(e, TALK(e)->m_szGrp[TLK_STARE], RANDOM_FLOAT(5, 7.5f), VOL_NORM, ATTN_IDLE);
	TALK(e)->m_hTalkTarget = EHANDLE(Tlk_FindNearestFriend(e, 1));
	return 1;
}

int Tlk_FIdleHello(int e)
{
	CTalkMonster	*t = TALK(e);
	int		pl;

	if (!Tlk_FOkToSpeak(e))
		return 0;
	if (!(t->m_bitsSaid & bit_saidHelloPlayer))
	{
		pl = Tlk_FindNearestFriend(e, 1);
		if (pl >= 0 && Mon_FInViewCone(e, pl) && FVisible(e, pl))
		{
			t->m_hTalkTarget = EHANDLE(pl);
			if (t->spawnflags & SF_MONSTER_PREDISASTER)
				Mon_PlaySentence(e, t->m_szGrp[TLK_PHELLO], RANDOM_FLOAT(3, 3.5f), VOL_NORM, ATTN_IDLE);
			else
				Mon_PlaySentence(e, t->m_szGrp[TLK_HELLO], RANDOM_FLOAT(3, 3.5f), VOL_NORM, ATTN_IDLE);
			t->m_bitsSaid |= bit_saidHelloPlayer;
			return 1;
		}
	}
	return 0;
}

int Tlk_FIdleSpeak(int e)
{
	CTalkMonster	*t = TALK(e);
	res_t		idle, question;
	float		duration;
	int		target, fr;

	if (!Tlk_FOkToSpeak(e))
		return 0;
	if (t->spawnflags & SF_MONSTER_PREDISASTER)
	{
		idle = t->m_szGrp[TLK_PIDLE];
		question = t->m_szGrp[TLK_PQUESTION];
		duration = RANDOM_FLOAT(4.8f, 5.2f);
	}
	else
	{
		idle = t->m_szGrp[TLK_IDLE];
		question = t->m_szGrp[TLK_QUESTION];
		duration = RANDOM_FLOAT(2.8f, 3.2f);
	}
	(void)Tlk_GetVoicePitch(e);
	target = EHGet(t->m_hTargetEnt);
	if (target >= 0 && IsPlayer(target) && IsAlive(target))
	{
		t->m_hTalkTarget = t->m_hTargetEnt;
		if (!(t->m_bitsSaid & bit_saidDamageHeavy) && ent_hp(target) <= ent_maxhp(target) / 8)
		{
			Mon_PlaySentence(e, t->m_szGrp[TLK_PLHURT3], duration, VOL_NORM, ATTN_IDLE);
			t->m_bitsSaid |= bit_saidDamageHeavy;
			return 1;
		}
		if (!(t->m_bitsSaid & bit_saidDamageMedium) && ent_hp(target) <= ent_maxhp(target) / 4)
		{
			Mon_PlaySentence(e, t->m_szGrp[TLK_PLHURT2], duration, VOL_NORM, ATTN_IDLE);
			t->m_bitsSaid |= bit_saidDamageMedium;
			return 1;
		}
		if (!(t->m_bitsSaid & bit_saidDamageLight) && ent_hp(target) <= ent_maxhp(target) / 2)
		{
			Mon_PlaySentence(e, t->m_szGrp[TLK_PLHURT1], duration, VOL_NORM, ATTN_IDLE);
			t->m_bitsSaid |= bit_saidDamageLight;
			return 1;
		}
	}
	fr = Tlk_FindNearestFriend(e, 0);
	if (fr >= 0 && !IsMoving(fr) && RANDOM_LONG(0, 99) < 75)
	{
		Mon_PlaySentence(e, question, duration, VOL_NORM, ATTN_IDLE);
		t->m_hTalkTarget = EHANDLE(fr);
		if (mon_vt(fr)->talk)
		{
			if (mon_vt(fr)->SetAnswerQuestion)
				mon_vt(fr)->SetAnswerQuestion(fr, e);
			else
				CTalkMonster_SetAnswerQuestion(fr, e);
			TALK(fr)->m_flStopTalkTime = t->m_flStopTalkTime;
		}
		t->m_nSpeak++;
		return 1;
	}
	if (RANDOM_LONG(0, 1))
	{
		fr = Tlk_FindNearestFriend(e, 1);
		if (fr >= 0)
		{
			t->m_hTalkTarget = EHANDLE(fr);
			Mon_PlaySentence(e, idle, duration, VOL_NORM, ATTN_IDLE);
			t->m_nSpeak++;
			return 1;
		}
	}
	Tlk_Talk(e, 0);
	g_talkWaitTime = 0;
	return 0;
}

void CTalkMonster_SetAnswerQuestion(int e, int speaker)
{
	CTalkMonster *t = TALK(e);

	if (EHGet(t->m_pCine) < 0)
		Mon_ChangeSchedule(e, slIdleResponse);
	t->m_hTalkTarget = EHANDLE(speaker);
}

void Tlk_TrySmellTalk(int e)
{
	CTalkMonster *t = TALK(e);

	if (!Tlk_FOkToSpeak(e))
		return;
	if (gtime() > t->m_flLastSaidSmelled)
		t->m_bitsSaid &= ~bit_saidSmelled;
	if (!(t->m_bitsSaid & bit_saidSmelled) && Mon_HasConditions((CBaseMonster *)t, bits_COND_SMELL))
	{
		Mon_PlaySentence(e, t->m_szGrp[TLK_SMELL], RANDOM_FLOAT(2.8f, 3.2f), VOL_NORM, ATTN_IDLE);
		t->m_flLastSaidSmelled = gtime() + 60;
		t->m_bitsSaid |= bit_saidSmelled;
	}
}

/* ---- tasks and schedules ------------------------------------------------------------ */

void CTalkMonster_StartTask(int e, const Task_t *task)
{
	CTalkMonster	*t = TALK(e);
	CBaseMonster	*m = (CBaseMonster *)t;
	int		tt;
	float		yaw, d[3], dir[3], mv[3], p[3];

	switch (task->iTask)
	{
	case TASK_TLK_SPEAK:
		Tlk_FIdleSpeak(e);
		Mon_TaskComplete(m);
		break;
	case TASK_TLK_RESPOND:
		Tlk_IdleRespond(e);
		Mon_TaskComplete(m);
		break;
	case TASK_TLK_HELLO:
		Tlk_FIdleHello(e);
		Mon_TaskComplete(m);
		break;
	case TASK_TLK_STARE:
		Tlk_FIdleStare(e);
		Mon_TaskComplete(m);
		break;
	case TASK_FACE_PLAYER:
	case TASK_TLK_LOOK_AT_CLIENT:
	case TASK_TLK_CLIENT_STARE:
		m->m_flWaitFinished = gtime() + task->flData;
		break;
	case TASK_TLK_EYECONTACT:
		break;
	case TASK_TLK_IDEALYAW:
		tt = EHGet(t->m_hTalkTarget);
		if (tt >= 0)
		{
			ent_phys(e)->yaw_speed = 60;
			OriginIn(tt, e, p);
			VecSub(p, ent_cv(e)->origin, d);
			yaw = Mon_VecToYaw(e, d) - ent_cv(e)->angles[1];
			if (yaw > 180)
				yaw -= 360;
			if (yaw < -180)
				yaw += 360;
			if (yaw < 0)
				ent_phys(e)->ideal_yaw = fminf(yaw + 45.0f, 0.0f) + ent_cv(e)->angles[1];
			else
				ent_phys(e)->ideal_yaw = fmaxf(yaw - 45.0f, 0.0f) + ent_cv(e)->angles[1];
		}
		Mon_TaskComplete(m);
		break;
	case TASK_TLK_HEADRESET:
		t->m_hTalkTarget = EH_NONE;
		Mon_TaskComplete(m);
		break;
	case TASK_TLK_STOPSHOOTING:
		Mon_PlaySentence(e, t->m_szGrp[TLK_NOSHOOT], RANDOM_FLOAT(2.8f, 3.2f), VOL_NORM, ATTN_NORM);
		Mon_TaskComplete(m);
		break;
	case TASK_CANT_FOLLOW:
		Mon_StopFollowing(e, 0);
		Mon_PlaySentence(e, t->m_szGrp[TLK_STOP], RANDOM_FLOAT(2, 2.5f), VOL_NORM, ATTN_NORM);
		Mon_TaskComplete(m);
		break;
	case TASK_WALK_PATH_FOR_UNITS:
		m->m_movementActivity = ACT_WALK;
		break;
	case TASK_MOVE_AWAY_PATH:
		VecCopy(ent_cv(e)->angles, dir);
		dir[1] = ent_phys(e)->ideal_yaw + 180;
		AngleVectors(dir, mv, 0, 0);
		VecMA(ent_cv(e)->origin, task->flData, mv, p);
		if (Mon_MoveToLocation(e, ACT_WALK, 2, p))
			Mon_TaskComplete(m);
		else
		{
			float org[3];

			VecCopy(ent_cv(e)->origin, org);
			if (Mon_FindCover(e, org, m->view_ofs, 0, Mon_CoverRadius(e)))
			{
				m->m_flMoveWaitFinished = gtime() + 2;
				Mon_TaskComplete(m);
			}
			else
				Mon_TaskFail(m);
		}
		break;
	case TASK_PLAY_SCRIPT:
		t->m_hTalkTarget = EH_NONE;
		CBaseMonster_StartTask(e, task);
		break;
	default:
		CBaseMonster_StartTask(e, task);
	}
}

void CTalkMonster_RunTask(int e, const Task_t *task)
{
	CTalkMonster	*t = TALK(e);
	CBaseMonster	*m = (CBaseMonster *)t;
	int		pl = ENT_PLAYER, tt;
	float		org[3], po[3];

	switch (task->iTask)
	{
	case TASK_TLK_CLIENT_STARE:
	case TASK_TLK_LOOK_AT_CLIENT:
		if (m->m_MonsterState == MONSTERSTATE_IDLE && !Mon_IsMoving(m) && !Tlk_IsTalking(e))
		{
			if (ent_is_live(pl))
			{
				OriginIn(pl, e, po);
				Tlk_IdleHeadTurn(e, po);
			}
		}
		else
		{
			Mon_TaskFail(m);
			return;
		}
		if (task->iTask == TASK_TLK_CLIENT_STARE && ent_is_live(pl))
		{
			OriginIn(pl, e, po);
			if (mv_dist2d(po, ent_cv(e)->origin) > TLK_STARE_DIST)
				Mon_TaskFail(m);
			UTIL_MakeVectors(ent_cv(pl)->angles);
			if (UTIL_DotPoints(po, ent_cv(e)->origin, gpGlobals.v_forward) <
				m->m_flFieldOfView)
				Mon_TaskFail(m);
		}
		if (gtime() > m->m_flWaitFinished)
			Mon_TaskComplete(m);
		break;
	case TASK_FACE_PLAYER:
		if (ent_is_live(pl))
		{
			OriginIn(pl, e, po);
			Mon_MakeIdealYaw(e, po);
			Mon_ChangeYaw(e, (int)ent_phys(e)->yaw_speed);
			Tlk_IdleHeadTurn(e, po);
			if (gtime() > m->m_flWaitFinished && Mon_FlYawDiff(e) < 10)
				Mon_TaskComplete(m);
		}
		else
			Mon_TaskFail(m);
		break;
	case TASK_TLK_EYECONTACT:
		tt = EHGet(t->m_hTalkTarget);
		if (!Mon_IsMoving(m) && Tlk_IsTalking(e) && tt >= 0)
		{
			OriginIn(tt, e, po);
			Tlk_IdleHeadTurn(e, po);
		}
		else
			Mon_TaskComplete(m);
		break;
	case TASK_WALK_PATH_FOR_UNITS:
		if (mv_dist2d(m->m_vecLastPosition, ent_cv(e)->origin) > task->flData ||
			Mon_MovementIsComplete(m))
		{
			Mon_TaskComplete(m);
			Mon_RouteClear(e);
		}
		break;
	case TASK_WAIT_FOR_MOVEMENT:
		tt = EHGet(t->m_hTalkTarget);
		VecCopy(ent_cv(e)->origin, org);
		if (Tlk_IsTalking(e) && tt >= 0)
		{
			OriginIn(tt, e, po);
			Tlk_IdleHeadTurn(e, po);
		}
		else
		{
			Tlk_IdleHeadTurn(e, org);
			Tlk_FIdleHello(e);
			if (RANDOM_LONG(0, t->m_nSpeak * 20) == 0)
				Tlk_FIdleSpeak(e);
		}
		CBaseMonster_RunTask(e, task);
		if (Mon_TaskIsComplete(m))
			Tlk_IdleHeadTurn(e, org);
		break;
	default:
		tt = EHGet(t->m_hTalkTarget);
		if (Tlk_IsTalking(e) && tt >= 0)
		{
			OriginIn(tt, e, po);
			Tlk_IdleHeadTurn(e, po);
		}
		else
			Mon_SetBoneController(e, 0, 0);
		CBaseMonster_RunTask(e, task);
	}
}

const Schedule_t *CTalkMonster_GetScheduleOfType(int e, int type)
{
	CTalkMonster	*t = TALK(e);
	CBaseMonster	*m = (CBaseMonster *)t;
	int		pl = ENT_PLAYER;
	float		po[3];

	switch (type)
	{
	case SCHED_MOVE_AWAY:
		return slMoveAway;
	case SCHED_MOVE_AWAY_FOLLOW:
		return slMoveAwayFollow;
	case SCHED_MOVE_AWAY_FAIL:
		return slMoveAwayFail;
	case SCHED_TARGET_FACE:
		if (RANDOM_LONG(0, 99) < 2)
			return slIdleSpeakWait;
		return slIdleStand;
	case SCHED_IDLE_STAND:
		if (!(t->m_bitsSaid & bit_saidHelloPlayer))
			return slIdleHello;
		if (!(t->m_bitsSaid & bit_saidWoundLight) && t->health <= t->max_health * 0.75f)
		{
			Mon_PlaySentence(e, t->m_szGrp[TLK_WOUND], RANDOM_FLOAT(2.8f, 3.2f), VOL_NORM, ATTN_IDLE);
			t->m_bitsSaid |= bit_saidWoundLight;
			return slIdleStand;
		}
		else if (!(t->m_bitsSaid & bit_saidWoundHeavy) && t->health <= t->max_health * 0.5f)
		{
			Mon_PlaySentence(e, t->m_szGrp[TLK_MORTAL], RANDOM_FLOAT(2.8f, 3.2f), VOL_NORM, ATTN_IDLE);
			t->m_bitsSaid |= bit_saidWoundHeavy;
			return slIdleStand;
		}
		if (Tlk_FOkToSpeak(e) && RANDOM_LONG(0, t->m_nSpeak * 2) == 0)
			return slIdleSpeak;
		if (!Tlk_IsTalking(e) && Mon_HasConditions(m, bits_COND_SEE_CLIENT) && RANDOM_LONG(0, 6) == 0)
		{
			if (ent_is_live(pl))
			{
				OriginIn(pl, e, po);
				UTIL_MakeVectors(ent_cv(pl)->angles);
				if (mv_dist2d(po, ent_cv(e)->origin) < TLK_STARE_DIST &&
					UTIL_DotPoints(po, ent_cv(e)->origin,
						gpGlobals.v_forward) >= m->m_flFieldOfView)
					return &slTlkIdleWatchClient[1];
				return slTlkIdleWatchClient;
			}
		}
		else
		{
			if (Tlk_IsTalking(e))
				return slTlkIdleEyecontact;
			return slIdleStand;
		}
		break;
	}
	return CBaseMonster_GetScheduleOfType(e, type);
}

void CTalkMonster_PrescheduleThink(int e)
{
	CBaseMonster *m = MON(e);

	if (!Mon_HasConditions(m, bits_COND_SEE_CLIENT))
		Mon_SetConditions(m, bits_COND_CLIENT_UNSEEN);
}

void CTalkMonster_HandleAnimEvent(int e, const MonsterEvent_t *ev)
{
	switch (ev->event)
	{
	case SCRIPT_EVENT_SENTENCE_RND1:
		if (RANDOM_LONG(0, 99) < 75)
			break;
		/* fall through */
	case SCRIPT_EVENT_SENTENCE:
		Tlk_ShutUpFriends(e);
		if (ev->res)
			Mon_PlaySentence(e, ev->res, RANDOM_FLOAT(2.8f, 3.4f), VOL_NORM, ATTN_IDLE);
		break;
	default:
		CBaseMonster_HandleAnimEvent(e, ev);
		break;
	}
}

/* ---- damage, death, touch, relationships ------------------------------------------------ */

int CTalkMonster_TakeDamage(int e, int inflictor, int attacker, float flDamage, int bits)
{
	int fr;

	if (IsAlive(e) && attacker >= 0 && MON(e)->m_MonsterState != MONSTERSTATE_PRONE &&
		(ent_cv(attacker)->flags & FL_CLIENT))
	{
		fr = Tlk_FindNearestFriend(e, 0);
		if (fr >= 0 && IsAlive(fr))
			Mon_ChangeSchedule(fr, slIdleStopShooting);
	}
	return CBaseMonster_TakeDamage(e, inflictor, attacker, flDamage, bits);
}

void CTalkMonster_Killed(int e, int attacker, int iGib)
{
	CBaseMonster *m = MON(e);

	if (attacker >= 0 && (ent_cv(attacker)->flags & FL_CLIENT) &&
		m->m_MonsterState != MONSTERSTATE_PRONE)
	{
		Tlk_AlertFriends(e);
		Tlk_LimitFollowers(e, attacker, 0);
	}
	m->m_hTargetEnt = EH_NONE;
	Tlk_StopTalking(e);
	SetUse(e, FN_NONE);
	CBaseMonster_Killed(e, attacker, iGib);
}

void CTalkMonster_Touch(int e, int other)
{
	CBaseMonster	*m = MON(e);
	float		speed;

	if (IsPlayer(other))
	{
		if (m->m_afMemory & bits_MEMORY_PROVOKED)
			return;
		if (Tlk_IsTalking(e))
			return;
		speed = fabsf(ent_mot(other)->velocity[0]) + fabsf(ent_mot(other)->velocity[1]);
		if (speed > 50)
		{
			Mon_SetConditions(m, bits_COND_CLIENT_PUSH);
			Mon_MakeIdealYaw(e, ent_cv(other)->origin);
		}
	}
}

int CTalkMonster_IRelationship(int e, int other)
{
	if (IsPlayer(other) && (MON(e)->m_afMemory & bits_MEMORY_PROVOKED))
		return R_HT;
	return CBaseMonster_IRelationship(e, other);
}

/* ---- following ------------------------------------------------------------------------- */

void CTalkMonster_StopFollowing(int e, int clearSchedule)
{
	CTalkMonster *t = TALK(e);

	if (Tlk_IsFollowing(e))
	{
		if (!(t->m_afMemory & bits_MEMORY_PROVOKED))
		{
			Mon_PlaySentence(e, t->m_szGrp[TLK_UNUSE], RANDOM_FLOAT(2.8f, 3.2f), VOL_NORM, ATTN_IDLE);
			t->m_hTalkTarget = t->m_hTargetEnt;
		}
		if (t->m_movementGoal == MOVEGOAL_TARGETENT)
			Mon_RouteClear(e);
		t->m_hTargetEnt = EH_NONE;
		if (clearSchedule)
			Mon_ClearSchedule(e);
		if (EHGet(t->m_hEnemy) >= 0)
			t->m_IdealMonsterState = MONSTERSTATE_COMBAT;
	}
}

void Tlk_StartFollowing(int e, int leader)
{
	CTalkMonster	*t = TALK(e);
	int		cine = EHGet(t->m_pCine);

	if (cine >= 0)
		Script_CancelScript(cine);
	if (EHGet(t->m_hEnemy) >= 0)
		t->m_IdealMonsterState = MONSTERSTATE_ALERT;
	t->m_hTargetEnt = EHANDLE(leader);
	Mon_PlaySentence(e, t->m_szGrp[TLK_USE], RANDOM_FLOAT(2.8f, 3.2f), VOL_NORM, ATTN_IDLE);
	t->m_hTalkTarget = t->m_hTargetEnt;
	Mon_ClearConditions((CBaseMonster *)t, bits_COND_CLIENT_PUSH);
	Mon_ClearSchedule(e);
}

int Tlk_CanFollow(int e)
{
	CTalkMonster	*t = TALK(e);
	int		cine = EHGet(t->m_pCine);

	if (t->m_MonsterState == MONSTERSTATE_SCRIPT)
	{
		if (cine < 0 || !Script_CanInterrupt(cine))
			return 0;
	}
	if (!IsAlive(e))
		return 0;
	return !Tlk_IsFollowing(e);
}

void CTalkMonster_FollowerUse(int e, int activator, int caller, int useType, float value)
{
	CTalkMonster *t = TALK(e);

	(void)activator; (void)useType; (void)value;
	if (t->m_useTime > gtime())
		return;
	if (caller >= 0 && IsPlayer(caller))
	{
		if (t->spawnflags & SF_MONSTER_PREDISASTER)
		{
			if (mon_vt(e)->DeclineFollowing)
				mon_vt(e)->DeclineFollowing(e);
		}
		else if (Tlk_CanFollow(e))
		{
			Tlk_LimitFollowers(e, caller, 1);
			if (!(t->m_afMemory & bits_MEMORY_PROVOKED))
			{
				Tlk_StartFollowing(e, caller);
				t->m_bitsSaid |= bit_saidHelloPlayer;
			}
		}
		else
			Mon_StopFollowing(e, 1);
	}
}

/* KeyValue (UseSentence / UnUseSentence) + Precache: custom +USE groups */
int Tlk_KeyValue(int e, gid_t16 gid)
{
	CTalkMonster *t = TALK(e);

	t->m_iszUse = gid != GID_NONE ? rec_key_res(gid, K_USESENTENCE) : RES_NONE;
	t->m_iszUnUse = gid != GID_NONE ? rec_key_res(gid, K_UNUSESENTENCE) : RES_NONE;
	return 0;
}

void Tlk_Precache(int e)
{
	CTalkMonster *t = TALK(e);

	if (t->m_iszUse)
		t->m_szGrp[TLK_USE] = t->m_iszUse;
	if (t->m_iszUnUse)
		t->m_szGrp[TLK_UNUSE] = t->m_iszUnUse;
}

void Tlk_TalkInit(int e)
{
	g_talkWaitTime = 0;
	TALK(e)->m_voicePitch = 100;
}
