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
 * sentences.c -- sentence groups (HLSDK dlls/sound.cpp USENTENCEG_* /
 * SENTENCEG_*), scripted_sentence (scripted.cpp CScriptedSentence) and
 * speaker (sound.cpp CSpeaker).
 *
 * Groups are the cooked rtb SGRP runs (rtb-format.md "Sentences and
 * groups"): group res_t -> first sentence id + count; "!GROUPn" is
 * sentence first + n. The no-repeat LRU per group is HLSDK's.
 * scripted_sentence is a logic record (HLE_CF_LOGIC): its CScriptedSentence
 * state lives in the record's rec_logic_t (flags: active) and its
 * FindThink / DelayThink are record timers; the other members are read
 * from the record's keys when used.
 */
#include <string.h>

#include "game/hl/monster.h"

/* ---- sentence groups ----------------------------------------------------------- */

#define SG_MAX		128	/* groups with an LRU (rtb GROUP ids below this) */

static uint8_t	sg_lru[SG_MAX][CSENTENCE_LRU_MAX];
static uint8_t	sg_init[SG_MAX];

void SENTENCEG_Init(void)
{
	memset(sg_init, 0, sizeof(sg_init));
}

static void InitLRU(uint8_t *plru, int count)
{
	int	i, j, k;
	uint8_t	t;

	if (count > CSENTENCE_LRU_MAX)
		count = CSENTENCE_LRU_MAX;
	for (i = 0; i < count; i++)
		plru[i] = (uint8_t)i;
	for (i = 0; i < count * 4; i++)
	{
		j = RANDOM_LONG(0, count - 1);
		k = RANDOM_LONG(0, count - 1);
		t = plru[j];
		plru[j] = plru[k];
		plru[k] = t;
	}
}

/* group res -> (first sentence, count); 0 when not a group */
static int group_of(res_t group, int *gi, uint16_t *first)
{
	int count;

	if (RES_KIND(group) != RES_K_GROUP)
		return 0;
	*gi = RES_INDEX(group);
	count = res_group((uint16_t)*gi, first);
	return count > 0 ? count : 0;
}

/* USENTENCEG_Pick -> ipick (-1 none); *sent = the sentence id */
static int USENTENCEG_Pick(res_t group, int *sent)
{
	int		gi, count, i;
	uint16_t	first;
	uint8_t		*plru;

	count = group_of(group, &gi, &first);
	if (!count)
		return -1;
	if (gi >= SG_MAX)
	{
		/* beyond the LRU table: plain random pick */
		i = RANDOM_LONG(0, count - 1);
		*sent = first + i;
		return i;
	}
	plru = sg_lru[gi];
	if (count > CSENTENCE_LRU_MAX)
		count = CSENTENCE_LRU_MAX;
	if (!sg_init[gi])
	{
		InitLRU(plru, count);
		sg_init[gi] = 1;
	}
	for (;;)
	{
		for (i = 0; i < count; i++)
		{
			if (plru[i] != 0xFF)
			{
				int ipick = plru[i];

				plru[i] = 0xFF;
				*sent = first + ipick;
				return ipick;
			}
		}
		InitLRU(plru, count);
	}
}

int SENTENCEG_Pick(res_t group)
{
	int s;

	return USENTENCEG_Pick(group, &s) >= 0 ? s : -1;
}

int SENTENCEG_PlayRndSz(int e, res_t group, float volume, float attn, int flags, int pitch)
{
	int s, ipick = USENTENCEG_Pick(group, &s);

	if (ipick >= 0)
		snd_sentence(e, CHAN_VOICE, (uint16_t)s, volume, attn, flags, pitch);
	return ipick;
}

int SENTENCEG_PlaySequentialSz(int e, res_t group, float volume, float attn, int flags, int pitch,
	int ipick, int freset)
{
	int		gi, count;
	uint16_t	first;

	count = group_of(group, &gi, &first);
	if (!count)
		return -1;
	if (ipick >= count)
		ipick = count - 1;
	snd_sentence(e, CHAN_VOICE, (uint16_t)(first + ipick), volume, attn, flags, pitch);
	if (ipick >= count)
		return freset ? 0 : count;
	return ipick + 1;
}

void SENTENCEG_Stop(int e, res_t group, int ipick)
{
	(void)group;
	if (ipick < 0)
		return;
	snd_stop(e, CHAN_VOICE);
}

/* ---- name-or-class searches (UTIL_FindEntityGeneric) ------------------------ */

static int find_generic(name_t n, cls_t cls, const float org[3], float radius)
{
	int	t, it = 0, best = -1;
	float	maxd2 = radius * radius, d;

	if (n != NAME_NONE && (t = rec_next_named(n, &it)) >= 0)
		return t;
	if (cls == CLS_NONE)
		return -1;
	if (cls == CLS_PLAYER)
		t = ent_is_live(ENT_PLAYER) ? ENT_PLAYER : -1;
	else
		t = ent_next_class(-1, cls, -1);
	while (t >= 0)
	{
		d = mv_dist(ent_cv(t)->origin, org);
		d = d * d;
		if (maxd2 > d)
		{
			best = t;
			maxd2 = d;
		}
		if (cls == CLS_PLAYER)
			break;
		t = ent_next_class(t, cls, -1);
	}
	return best;
}

/* ---- scripted_sentence ---------------------------------------------------------- */

#define SF_SENTENCE_ONCE	0x0001
#define SF_SENTENCE_FOLLOWERS	0x0002
#define SF_SENTENCE_INTERRUPT	0x0004
#define SF_SENTENCE_CONCURRENT	0x0008

#define SS_ACTIVE		1
#define SS_T_FIND		1	/* timer args */
#define SS_T_DELAY		2

static float ss_attenuation(gid_t16 gid)
{
	switch ((int)KeyF(gid, K_ATTENUATION, 0))
	{
	case 1: return ATTN_STATIC;
	case 2: return ATTN_NORM;
	case 3: return ATTN_NONE;
	default: return ATTN_IDLE;
	}
}

static float ss_volume(gid_t16 gid)
{
	float v = KeyF(gid, K_VOLUME, 0) * 0.1f;

	return v <= 0 ? 1.0f : v;
}

static int AcceptableSpeaker(gid_t16 gid, int t)
{
	uint32_t	sf = (uint32_t)KeyF(gid, K_SPAWNFLAGS, 0);
	int		leader;

	if (t >= 0 && mon_is_monster(t) && t != ENT_PLAYER)
	{
		if (sf & SF_SENTENCE_FOLLOWERS)
		{
			leader = EHGet(MON(t)->m_hTargetEnt);
			if (leader < 0 || !IsPlayer(leader))
				return 0;
		}
		return Mon_CanPlaySentence(t, (sf & SF_SENTENCE_INTERRUPT) != 0);
	}
	return 0;	/* CBaseEntity::IsAllowedToSpeak: FALSE */
}

static int ss_find_entity(gid_t16 gid, const float org[3])
{
	name_t	n = rec_key_name(gid, K_ENTITY);
	cls_t	cls = rec_key_cls(gid, K_ENTITY);
	float	radius = KeyF(gid, K_RADIUS, 0);
	int	t, it = 0;

	if (n != NAME_NONE)
		while ((t = rec_next_named(n, &it)) >= 0)
			if (ent_priv(t) && AcceptableSpeaker(gid, t))
				return t;
	if (cls != CLS_NONE)
	{
		t = -1;
		while ((t = UTIL_FindEntityInSphere(t, org, radius)) >= 0)
			if (ent_idc(t)->cls == cls && (ent_cv(t)->flags & FL_MONSTER) &&
				AcceptableSpeaker(gid, t))
				return t;
	}
	return -1;
}

/* the logic record's SUB_UseTargets (CBaseDelay: delay, killtarget, target) */
static void logic_use_targets(gid_t16 gid, int activator)
{
	name_t	target = rec_key_name(gid, K_TARGET), kill = rec_key_name(gid, K_KILLTARGET);
	float	delay = KeyF(gid, K_DELAY, 0);

	if (delay != 0)
	{
		rec_fire_at(gtime() + delay, target, kill, activator, -1, USE_TOGGLE, 0);
		return;
	}
	if (kill != NAME_NONE)
		KillTargets(kill);
	if (target != NAME_NONE)
		rec_fire(target, activator, -1, gid, USE_TOGGLE, 0);
}

static void StartSentence(gid_t16 gid, int t)
{
	uint32_t	sf = (uint32_t)KeyF(gid, K_SPAWNFLAGS, 0);
	res_t		sent = rec_key_res(gid, K_SENTENCE);
	name_t		ln = rec_key_name(gid, K_LISTENER);
	cls_t		lc = rec_key_cls(gid, K_LISTENER);
	float		radius = KeyF(gid, K_RADIUS, 0);
	int		listener = -1;

	if (ln != NAME_NONE || lc != CLS_NONE)
	{
		if (lc == CLS_PLAYER)
			radius = 4096;
		listener = find_generic(ln, lc, ent_cv(t)->origin, radius);
	}
	if (RES_KIND(sent) == RES_K_SENTENCE)
	{
		Mon_PlayScriptedSentence(t, sent, KeyF(gid, K_DURATION, 0), ss_volume(gid),
			ss_attenuation(gid), !(sf & SF_SENTENCE_CONCURRENT), listener);
		log_printf("sentence: %s by %d t=%.2f\n", res_name(sent), t, gtime());
	}
	logic_use_targets(gid, -1);
}

static void ss_find_think(gid_t16 gid)
{
	rec_logic_t	*l = rec_logic(gid);
	float		org[3];
	int		t;

	rec_origin(gid, org);
	t = ss_find_entity(gid, org);
	if (t >= 0)
	{
		StartSentence(gid, t);
		if ((uint32_t)KeyF(gid, K_SPAWNFLAGS, 0) & SF_SENTENCE_ONCE)
		{
			rec_timer_cancel(gid);
			rec_kill(gid);
			return;
		}
		rec_timer(gid, gtime() + KeyF(gid, K_DURATION, 0) + KeyF(gid, K_REFIRE, 0), SS_T_DELAY, -1);
		l->flags &= ~SS_ACTIVE;
	}
	else
		rec_timer(gid, gtime() + KeyF(gid, K_REFIRE, 0) + 0.5f, SS_T_FIND, -1);
}

static void CScriptedSentence_LogicSpawn(gid_t16 gid)
{
	rec_logic_t *l = rec_logic(gid);

	l->flags = SS_ACTIVE;
	if (rec_targetname(gid) == NAME_NONE)
		rec_timer(gid, gtime() + 1.0f, SS_T_FIND, -1);
}

static void CScriptedSentence_LogicUse(gid_t16 gid, int activator, int caller, int use_type,
	float value)
{
	rec_logic_t *l = rec_logic(gid);

	(void)activator; (void)caller; (void)use_type; (void)value;
	if (!(l->flags & SS_ACTIVE))
		return;
	rec_timer_cancel(gid);
	rec_timer(gid, gtime(), SS_T_FIND, -1);
}

static void CScriptedSentence_LogicTimer(gid_t16 gid, uint16_t arg, int activator)
{
	rec_logic_t *l = rec_logic(gid);

	(void)activator;
	if (arg == SS_T_FIND)
		ss_find_think(gid);
	else
	{
		/* DelayThink */
		l->flags |= SS_ACTIVE;
		if (rec_targetname(gid) == NAME_NONE)
			rec_timer(gid, gtime() + 0.1f, SS_T_FIND, -1);
	}
}

const game_class_t gc_SCRIPTED_SENTENCE = {
	.LogicSpawn = CScriptedSentence_LogicSpawn,
	.LogicUse = CScriptedSentence_LogicUse,
	.LogicTimer = CScriptedSentence_LogicTimer,
};

